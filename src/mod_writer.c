#include <stdio.h>
#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"


BOOLEAN write_pages_to_disc(VOID)
{
    ULONG64 target_pages;
    PFN_LIST batch_list;
    PPFN pfn;
    PFN local;

    ULONG64 start_time = GetTickCount64();

    // Find the target number of pages to write
    target_pages = MAX_MOD_BATCH;

    // There is no reason to get more pages than we have disc indices
    // This check is done without locks, so it is not perfectly accurate
    // Still, it will give us a good enough heuristic of our supply of modified pages
    if (modified_page_list.num_pages < target_pages)
    {
        target_pages = modified_page_list.num_pages;
    }

    // Get as many disc indices as we can
    ULONG64 disc_indices[MAX_MOD_BATCH];
    ULONG num_returned_indices = get_disc_indices(disc_indices, target_pages);
    if (num_returned_indices == 0)
    {
        return FALSE;
    }

    // Bound the number of pages to pull off the modified list by the number of disc indices we have
    target_pages = num_returned_indices;

    // Initialize the list of pages to write to disc
    initialize_listhead(&batch_list);
    batch_list.num_pages = 0;

    // Lock the list of modified pages
    EnterCriticalSection(&modified_page_list.lock);

    // Check the count of the list. If the list is empty, we can return after freeing the disc indices
    if (modified_page_list.num_pages == 0)
    {
        LeaveCriticalSection(&modified_page_list.lock);
        free_disc_indices(disc_indices, num_returned_indices, 0);
        return FALSE;
    }

    // Pop the modified pages
    batch_pop_from_list_head(&modified_page_list, &batch_list, target_pages, TRUE);
    LeaveCriticalSection(&modified_page_list.lock);

    target_pages = batch_list.num_pages;

    if (batch_list.num_pages == 0)
    {
        free_disc_indices(disc_indices, num_returned_indices, 0);
        return FALSE;
    }

    // Find the frame numbers associated with the PFNs
    ULONG64 frame_numbers[MAX_MOD_BATCH];
    PLIST_ENTRY entry = batch_list.entry.Flink;
    for (ULONG64 i = 0; i < target_pages; i++)
    {
        pfn = CONTAINING_RECORD(entry, PFN, entry);
        frame_numbers[i] = frame_number_from_pfn(pfn);
        entry = entry->Flink;
    }

    // Free any extra disc indices
    if (batch_list.num_pages < num_returned_indices)
    {
        free_disc_indices(disc_indices, num_returned_indices, batch_list.num_pages);
    }

    // Map the pages to our private VA space
    map_pages(modified_write_va, target_pages, frame_numbers);

    // For each page, copy the contents to the paging file according to its corresponding disc index
    for (ULONG64 i = 0; i < target_pages; i++)
    {
        // Write the page to the page file
        write_to_pagefile(disc_indices[i], modified_write_va);

        // Lock the PFN. Change is possible as we are writing to the page file
        pfn = pfn_from_frame_number(frame_numbers[i]);
        lock_pfn(pfn);
        local = read_pfn(pfn);

        // The modified bit allows us to tell whether the page was changed during the write
        // Without the bit a page that went to active and then back to modified could not be differentiated
        // From one that was never touched. If a page was written to, it could be written twice
        // And its first page file write would be stale data
        if (local.flags.modified == 0) {
            local.disc_index = disc_indices[i];
            local.flags.state = STANDBY;
            local.flags.reference -= 1;
            write_pfn(pfn, local);
        }
        // In this case we know that the page in memory was written during our pagefile write
        // We have to throw away our page file space as it is stale data now
        else {
            local.flags.reference -= 1;
            local.flags.modified = 0;
            write_pfn(pfn, local);

            frame_numbers[i] = 0;
            free_disc_index(disc_indices[i]);
        }
        // Once reads are implemented, the write is still good and we should keep the copy on the disc.
        // We just decrement our reference count
    }

    unmap_pages(modified_write_va, target_pages);

    EnterCriticalSection(&standby_page_list.lock);

    // Add the pages to the standby list
    link_list_to_tail(&standby_page_list, &batch_list);

    LeaveCriticalSection(&standby_page_list.lock);

    for (ULONG64 i = 0; i < target_pages; i++)
    {
        // If the frame number is zero, it means that the page was written to and we freed the disc index
        if (frame_numbers[i] != 0)
        {
            // Unlock the PFN so that it can be used again
            unlock_pfn(pfn_from_frame_number(frame_numbers[i]));
        }
    }

    // Signal to other threads that pages are available
    SetEvent(pages_available_event);

    ULONG64 end_time = GetTickCount64();
    ULONG64 duration = end_time - start_time;

    track_mod_write_time(duration, target_pages);
    return TRUE;
}


// This controls the thread that constantly writes pages to disc when prompted by other threads
// In the future this should use a fraction of the CPU if the system cannot give it a full core
DWORD modified_write_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    // This thread needs to be able to react to handles for waking as well as exiting
    HANDLE handles[2];

    handles[0] = system_exit_event;
    handles[1] = modified_writing_event;

    // This waits for the system to start before doing anything
    WaitForSingleObject(system_start_event, INFINITE);
    set_modified_status("modified write thread started");

    while (TRUE)
    {
        ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 1000);
        if (index == 0)
        {
            set_modified_status("modified write thread exited");
            break;
        }

        ULONG64 batches = *(volatile ULONG64 *) (&num_batches_to_write);
        for (ULONG64 i = 0; i < batches; i++) {
            // If there are no pages to be written, indicated by returning false
            // Then the thread waits to be woken up again and retry
            if (write_pages_to_disc() == FALSE)
            {
                break;
            }
        }
    }

    return 0;
}
