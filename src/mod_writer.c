#include <stdio.h>
#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"

VOID write_pages_to_disc(PULONG64 target_writes)
{
    ULONG64 target_pages;
    PFN_LIST batch_list;
    PPFN pfn;
    PFN local;

    TIME_COUNTER time_counter;
    start_counter(&time_counter);

    // Find the target number of pages to write
    if (*target_writes > MAX_MOD_BATCH)
    {
        target_pages = MAX_MOD_BATCH;
    }
    else
    {
        target_pages = *target_writes;
    }

    // Get as many disc indices as we can
    ULONG64 disc_indices[MAX_MOD_BATCH];
    ULONG num_returned_indices = get_disc_indices(disc_indices, target_pages);
    if (num_returned_indices == 0)
    {
        // TODO Figure out if we want to track this
        return;
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
        return;
    }

    // Pop the modified pages
    batch_pop_from_list_head(&modified_page_list, &batch_list, target_pages, TRUE);
    LeaveCriticalSection(&modified_page_list.lock);

    target_pages = batch_list.num_pages;

    if (batch_list.num_pages == 0)
    {
        free_disc_indices(disc_indices, num_returned_indices, 0);
        return;
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

        // The dirtied bit allows us to tell whether the page was changed during the write
        // Without the bit a page that went to active and then back to modified could not be differentiated
        // From one that was never touched. If a page was written to, it could be written twice
        // And its first page file write would be stale data
        if (local.flags.dirtied == 0) {
            local.disc_index = disc_indices[i];
            local.flags.state = STANDBY;
            local.flags.reference -= 1;
            write_pfn(pfn, local);
        }
        // In this case we know that the page in memory was written during our page file write
        // We have to throw away our page file space as it is stale data now
        else {
            local.flags.reference -= 1;
            local.flags.dirtied = 0;
            write_pfn(pfn, local);

            // TODO have a second batch list to put back on to modified
            // TODO check the state of the pfn. if it is active then dont put it on any list
            // TODO If it is dangling (new state) then put it on the modified list
            assert(FALSE)
            //remove_from_list(pfn);

            frame_numbers[i] = 0;
            free_disc_index(disc_indices[i]);
        }
        // Once reads are implemented, the write is still good, and we should keep the copy on the disc.
        // We just decrement our reference count
    }

    unmap_pages(modified_write_va, target_pages);

    EnterCriticalSection(&standby_page_list.lock);

    // Add the pages to the standby list
    link_list_to_tail(&standby_page_list, &batch_list);

    LeaveCriticalSection(&standby_page_list.lock);

    for (ULONG64 i = 0; i < target_pages; i++)
    {
        // If the frame number is zero, it means that the page was written to, and we freed the disc index
        if (frame_numbers[i] != 0)
        {
            // Unlock the PFN so that it can be used again
            unlock_pfn(pfn_from_frame_number(frame_numbers[i]));
        }
    }

    // Signal to other threads that pages are available
    SetEvent(pages_available_event);

    stop_counter(&time_counter);
    DOUBLE duration = get_counter_duration(&time_counter);

    track_time(duration, target_pages, mod_write_times,
               &mod_write_time_index, MOD_WRITE_TIMES_TO_TRACK);

    // Decrease the target writes by the number of pages we wrote
    if (*target_writes < target_pages) {
        *target_writes = 0;
    }
    else {
        *target_writes -= target_pages;
    }
}


// This controls the thread that constantly writes pages to disc when prompted by other threads
// In the future this should use a fraction of the CPU if the system cannot give it a full core
DWORD modified_write_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    // This thread needs to be able to react to handles for waking as well as exiting
    HANDLE handles[2];

    handles[0] = system_exit_event;
    handles[1] = mw_wake_event;

    // This waits for the system to start before doing anything
    WaitForSingleObject(system_start_event, INFINITE);
    set_modified_status("modified write thread started");

    while (TRUE)
    {
        ULONG64 num_mod_writes = 0;

        ULONG64 average_page_consumption = average_page_consumption_global;

        ULONG64 consumable_pages = *(volatile ULONG_PTR *) (&free_page_list.num_pages) +
                                         *(volatile ULONG_PTR *) (&standby_page_list.num_pages);

        DOUBLE time_until_no_pages = (DOUBLE) consumable_pages / (DOUBLE) average_page_consumption;

        // Find how long it takes to write a page to disc
        TIME_MEASURE mw_average = average_tracked_times(mod_write_times, ARRAYSIZE(mod_write_times));
        global_mw_average = *(volatile TIME_MEASURE *) (&mw_average);

        DOUBLE mw_per_page_cost = mw_average.duration / (DOUBLE) mw_average.num_pages;

        // Find how long it will take us to empty our modified list completely and convert to seconds
        ULONG64 modified_pages = *(volatile ULONG64 *) (&modified_page_list.num_pages);
        DOUBLE time_to_mw = (DOUBLE) modified_pages * mw_per_page_cost;

        if (time_until_no_pages < time_to_mw) {
            // If we don't have enough time to empty the modified list, write constantly
            num_mod_writes = modified_pages;
        } else {
            // Wait for the events only if we've actively checked if we need to write modified pages
            ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 1000);
            if (index == 0)
            {
                set_modified_status("modified write thread exited");
                break;
            }
        }

        while (num_mod_writes > 0) {
            write_pages_to_disc(&num_mod_writes);
        }
    }

    return 0;
}
