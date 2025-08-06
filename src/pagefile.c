#include <stdio.h>
#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"

HANDLE pagefile_handle;
PVOID page_file;


PBITMAP_CHUNK page_file_bitmap;
PBITMAP_CHUNK page_file_bitmap_end;
volatile LONG64 free_disc_spot_count;

PULONG64 freed_spaces;
volatile LONG64 freed_spaces_size;

volatile LONG64 last_checked_index;


ULONG64 add_freed_index(ULONG64 disc_index);
ULONG64 get_freed_index();

ULONG64 disc_index_to_region(ULONG64 disc_index)
{
    ULONG64 region = disc_index / ((ULONG64) BITMAP_REGION_SIZE_IN_BYTES * BITMAP_REGION_SIZE_IN_BITS);
    assert(region < BITMAP_SIZE_IN_REGIONS);
    return region;
}

// Called with the freed_spaces_lock and the region lock held
// Returns the updated ULONG64 with the bits set to 1
VOID search_chunk_for_free_spots(PULONG64 disc_spot, ULONG64 first_index, PULONG count, PULONG64 disc_indices, ULONG64 num_indices)
{
    ULONG64 expected = *disc_spot;

    // Variables for the compare exchange
    ULONG64 desired = FULL_BITMAP_CHUNK;
    ULONG64 compex_return;


    // Fill out the entire chunk
    while (TRUE) {
        compex_return = InterlockedCompareExchange64((volatile PLONG64) disc_spot, desired, expected);

        // Check if the ULONG64 has changed since we last read it
        if (compex_return == expected) {
            break;
        }

        // If it was changed to FULL_BITMAP_CHUNK, then we can't add any more indices
        if (compex_return == FULL_BITMAP_CHUNK) {
            return;
        }

        expected = compex_return;
    }

    ULONG64 disc_spot_local = desired & ~expected;
    ULONG64 add_result;

    // Iterate over the old value and find the indices of all the free spots we just filled
    for (int bit = 0; bit < BITMAP_CHUNK_SIZE_IN_BITS; bit++)
    {
        ULONG64 bit_mask = (FULL_UNIT << bit);
        if ((disc_spot_local & bit_mask) != EMPTY_UNIT) {

            if (*count < num_indices) {
                // Add the index to disc_indices
                disc_indices[*count] = first_index + bit;
                count++;
            } else{
                add_result = add_freed_index(first_index + bit);
                if (add_result == DISC_INDEX_FAIL_CODE) {
                    break;
                }
            }
            disc_spot_local &= ~bit_mask;
        }
    }
    // At the end of this, disc_spot_local equals every bit we could not consume
    disc_spot_local = ~disc_spot_local;
    InterlockedAnd64((volatile PLONG64) disc_spot, disc_spot_local);

    LONG64 last_checked_index_local = first_index;
    InterlockedExchange64(&last_checked_index, last_checked_index_local);
}

// Called with the freed_spaces_lock held, only needs to worry about the bitmap region lock
VOID search_region_for_free_spots(ULONG64 region, PULONG count, PULONG64 disc_indices, ULONG64 num_indices)
{
    // No lock is gotten here to make reading faster, we only need it to write once we find a free spot

    // For each char in the region
    PULONG64 disc_region_base = page_file_bitmap + region * BITMAP_REGION_SIZE_IN_BYTES / BITMAP_CHUNK_SIZE;
    PULONG64 disc_region_end = disc_region_base + BITMAP_REGION_SIZE_IN_BYTES / BITMAP_CHUNK_SIZE;

    for (PULONG64 disc_spot = disc_region_base; disc_spot < disc_region_end; disc_spot++)
    {
        if (*disc_spot == FULL_BITMAP_CHUNK)
        {
            continue;
        }

        // Compute the lowest disc index
        ULONG64 lowest_disc_index = (region * BITMAP_REGION_SIZE_IN_BYTES) + (disc_spot - disc_region_base) * BITMAP_CHUNK_SIZE;
        lowest_disc_index *= BITS_PER_BYTE;

        search_chunk_for_free_spots(disc_spot, lowest_disc_index, count, disc_indices, num_indices);

        if (*count == num_indices)
        {
            // We have found enough indices, return
            return;
        }
    }
}


ULONG64 get_disc_indices(PULONG64 disc_indices, ULONG64 num_indices)
{
    ULONG count = 0;
    ULONG64 return_index;

    // If our free_disc_spot_count is zero, we can't get any indices and return without checking the bitmap
    // Volatile read is needed here for two reasons
    // 1. Breaking up the read. Without it, the compiler could break up the read into pieces and corrupt the contents
    // If another thread is changing this value at the same time, the pieces won't fit anymore
    // 2. Rereading. The compiler can substitute this read line at every single reference to this variable.
    // If it changes in the gap, we think we have a snapshot but we don't
    if (*(volatile ULONG_PTR *) (&free_disc_spot_count) == 0)
    {
        return 0;
    }

    // Tries to get as many disc indices from the freed spaces stack as possible
    while (count < num_indices)
    {
        return_index = get_freed_index();
        if (return_index != DISC_INDEX_FAIL_CODE)
        {
            disc_indices[count] = return_index;
            count++;
        }
        else {
            break;
        }
    }

    // If we have satisfied our count, return
    if (count == num_indices)
    {
        // Decrement the free_disc_spot_count by the number of indices we got
        InterlockedAdd64(&free_disc_spot_count, 0 - (ULONG64) count);
        return count;
    }

    // Iterate over each bitmap region looking for free spots
    ULONG64 first_bitmap_region = disc_index_to_region(last_checked_index);

    for (ULONG64 region = 0; region < BITMAP_SIZE_IN_REGIONS; region++)
    {
        ULONG64 search_region = (first_bitmap_region + region) % BITMAP_SIZE_IN_REGIONS;
        search_region_for_free_spots(search_region, &count, disc_indices, num_indices);
        if (count == num_indices)
        {
            // We have found enough indices, break
            break;
        }
    }

    // If we didn't break, we found less than num_indices indices and return how many we actually got
    // If we did break, we got num_indices indices and return that
    InterlockedAdd64(&free_disc_spot_count, 0 - (ULONG64) count);
    return count;
}

// This function will double insert a disc index if it is called twice with the same index
// Currently this is not a problem, as this function is only called with locks held that prevent this from happening
VOID free_disc_index(ULONG64 disc_index)
{
    PULONG64 disc_spot;
    ULONG64 index_in_cluster;

    // Add the disc index to freed_spaces if there is space
    if (add_freed_index(disc_index) == DISC_INDEX_FAIL_CODE) {
        // This grabs the actual chunk (ULONG64) that holds the bit we need to change
        disc_spot = page_file_bitmap + disc_index / BITMAP_CHUNK_SIZE_IN_BITS;

        // This gets the bit's index inside the char
        index_in_cluster = disc_index % BITMAP_CHUNK_SIZE_IN_BITS;

        ULONG64 mask = ~(FULL_UNIT << index_in_cluster);

        InterlockedAnd64((volatile PLONG64) disc_spot, mask);

        // We set the bit to be zero by comparing it using a LOGICAL AND (&=) with all ones,
        // Except for a zero at the place we want to set as zero.
        // We compute this comparison value by taking one positive bit (1)
        // And left-shifting (<<) it by index_in_cluster bits to its corresponding position in the char
        // If the position is two, then our 1 would become 001. Five more bits would then be added to the end
        // By the compiler in order to match the size of the char it is being compared to (00100000)
        // Then we flip these bits using the (~) operator to get our comparison value

        // Example: (actual byte) 11011101 &= 11111011 (comparison value)
        // Result: 11011(0)01
        // The zero surrounded by parenthesis is the bit we change; all others are preserved

        // This asserts that the disc space is not already free
        //assert((spot_cluster & (FULL_UNIT << (index_in_cluster))) != EMPTY_UNIT);

        // If there is no space, then we want to move our last checked index back to the on_stack index so that we don't
        // lose it, as it will otherwise sit in a bubble of all the spaces before last_checked_index
        if (disc_index < last_checked_index)
        {
            InterlockedExchange64(&last_checked_index, disc_index);
        }
    }

    InterlockedIncrement64(&free_disc_spot_count);
    SetEvent(disc_spot_available_event);
}

// This will only be slower in the rare edge case that the indices are in the same disc spot as a compare exchange will
// be done multiple times on it. This needs to be though about.
// Will sorting and matching to disc spots be worth it in the end?
VOID free_disc_indices(PULONG64 disc_indices, ULONG64 num_indices, ULONG64 start_index)
{
    for (ULONG64 i = start_index; i < num_indices; i++)
    {
        free_disc_index(disc_indices[i]);
    }
}

ULONG64 add_freed_index(ULONG64 disc_index)
{
    ULONG64 expected = freed_spaces_size;
    if (expected == MAX_FREED_SPACES_SIZE)
    {
        return DISC_INDEX_FAIL_CODE;
    }
    ULONG64 desired = expected + 1;
    ULONG64 compex_return = ~expected;
    while (compex_return != expected)
    {
        compex_return = InterlockedCompareExchange64((volatile PLONG64) &freed_spaces_size, desired, expected);

        if (compex_return != expected)
        {
            expected = compex_return;
            if (expected == MAX_FREED_SPACES_SIZE)
            {
                return DISC_INDEX_FAIL_CODE;
            }
            desired = expected + 1;
        }
        else
        {
            break;
        }
    }

    freed_spaces[compex_return] = disc_index;
    return disc_index;
}

// Get a disc index from the freed spaces stack
// Does not decrement the free_disc_spot_count, as this is done by the caller
ULONG64 get_freed_index()
{
    ULONG64 expected = freed_spaces_size;
    if (expected == 0)
    {
        return DISC_INDEX_FAIL_CODE;
    }
    ULONG64 desired = expected - 1;
    ULONG64 compex_return = ~expected;
    while (compex_return != expected)
    {
        compex_return = InterlockedCompareExchange64((volatile PLONG64) &freed_spaces_size, desired, expected);

        if (compex_return != expected)
        {
            expected = compex_return;
            if (expected == 0)
            {
                return DISC_INDEX_FAIL_CODE;
            }
            desired = expected - 1;
        }
        else
        {
            break;
        }
    }
    ULONG64 index = freed_spaces[compex_return - 1];

    return index;
}

VOID read_from_pagefile(ULONG64 disc_index, PVOID dst_va) {
    PVOID file_view = (char*) page_file + disc_index * PAGE_SIZE;
    memcpy(dst_va, file_view, PAGE_SIZE);
}

// TODO flush once after all writes
VOID write_to_pagefile(ULONG64 disc_index, PVOID src_va) {
    PVOID file_view = (char*) page_file + disc_index * PAGE_SIZE;
    memcpy(file_view, src_va, PAGE_SIZE);

    if (!FlushViewOfFile(file_view, PAGE_SIZE)) {
        fatal_error("Failed to flush view of file");
    }
}