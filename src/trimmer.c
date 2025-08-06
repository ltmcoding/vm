#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"

#define TRIM_ALL      MAXULONG64

// TODO don't put everything in modified, reference has to be zero. still trim the page make it dangling state
VOID trim_pte_region(PULONG64 target_trims) {
    PFN_LIST batch_list;
    ULONG trim_batch_size = 0;
    PTE_REGION_AGE_COUNT local_count = {0};
    PVOID virtual_addresses[PTE_REGION_SIZE];
    PPFN pfn;
    PPTE_REGION region;

    TIME_COUNTER time_counter;
    start_counter(&time_counter);

    GLOBAL_AGE_COUNT age_snapshot = *(volatile GLOBAL_AGE_COUNT *) &global_age_count;
    ULONG64 desired_trims = *target_trims;
    GLOBAL_AGE_COUNT trim_of_age = {0};

    for (LONG i = NUMBER_OF_AGES - 1; i >= 0; i--) {
        // If we want to trim more pages than we have in this age, we will take all the pages of this age
        if (desired_trims > age_snapshot.pages_of_age[i]) {
            trim_of_age.pages_of_age[i] = TRIM_ALL;
            desired_trims -= age_snapshot.pages_of_age[i];
        }
        // Otherwise we only want to trim the number of pages we want and break
        else {
            trim_of_age.pages_of_age[i] = desired_trims;
            break;
        }
    }

    // Try to find how man
    LONG age = NUMBER_OF_AGES - 1;
    while (age >= 0) {
        // TODO don't take off the list just yet
        EnterCriticalSection(&pte_region_age_lists[age].lock);
        region = pop_region_from_list(&pte_region_age_lists[age]);
        LeaveCriticalSection(&pte_region_age_lists[age].lock);

        if (region == NULL) {
            age--;
            continue;
        }
        break;
    }

    // No regions to trim, return 0
    if (region == NULL) {
        return;
    }

    PTE_REGION_AGE_COUNT old_count = *(volatile PTE_REGION_AGE_COUNT *) &region->age_count;

    initialize_listhead(&batch_list);

    // Grab the first PTE in the region
    PPTE current_pte = pte_from_pte_region(region);
    for (ULONG index = 0; index < PTE_REGION_SIZE; index++) {
        // Break early if we reach the end of the PTEs
        // TODO just make the last region full so we don't have to check this
        if (current_pte == pte_end)
        {
            break;
        }
        // Check the valid bit of the PTE
        if (current_pte->memory_format.valid == 1) {
            if (current_pte->memory_format.accessed == 1) {
                // If the PTE is accessed, we reset its age
                PTE local = read_pte(current_pte);
                local.memory_format.age = 0;
                local.memory_format.accessed = 0;
                write_pte(current_pte, local);
                increase_age_count(&local_count, 0);
                current_pte++;
                continue;
            }
            // If the thread is allowed to trim this PTE, we will do so
            if (trim_of_age.pages_of_age[current_pte->memory_format.age] > 0) {
                pfn = pfn_from_frame_number(current_pte->memory_format.frame_number);
                lock_pfn(pfn);

                if (pfn->flags.reference != 0) {
                    // If the PFN is referenced, we cannot trim it
                    unlock_pfn(pfn);
                    increase_age_count(&local_count, current_pte->memory_format.age);
                    current_pte++;
                    continue;
                }
                // Add it to the list of pages to unmap and trim
                add_to_list_head(pfn, &batch_list);
                virtual_addresses[trim_batch_size] = va_from_pte(current_pte);
                NULL_CHECK(virtual_addresses[trim_batch_size], "trim : could not get the va connected to the pte")
                trim_batch_size++;
                trim_of_age.pages_of_age[current_pte->memory_format.age]--;

            }
            // Otherwise, it stays active, and the thread needs to recount its age for the PTE region
            else {
                increase_age_count(&local_count, current_pte->memory_format.age);
            }
        }
        current_pte++;
    }

    assert(trim_batch_size == batch_list.num_pages)
    // If we did not trim any pages, we can skip the rest of the processing and break out
    // We only expect this to happen in edge cases where the ages of every trimmable page in the region
    // Are reset by memory accesses
    if (trim_batch_size == 0) {
        for (ULONG i = 0; i < NUMBER_OF_AGES; i++)
        {
            WriteUShortNoFence(&region->age_count.ages[i], local_count.ages[i]);
        }
        // Find the oldest age to insert the region back into the age lists
        // Set oldest age to a value that is larger than any possible age
        // If it stays that way, it means that no active pages remain in the region
        ULONG oldest_age = NUMBER_OF_AGES;
        for (ULONG i = 0; i < NUMBER_OF_AGES; i++) {
            if (local_count.ages[i] != 0) {
                oldest_age = i;
            }
        }

        // If there are no active pages left in the region, we can make it inactive, unlock, and return
        if (oldest_age == NUMBER_OF_AGES) {
            make_region_inactive(region);
            unlock_pte_region(region);
        } else {
            PPTE_REGION_LIST new_listhead = &pte_region_age_lists[oldest_age];

            EnterCriticalSection(&new_listhead->lock);
            add_region_to_list(region, new_listhead);
            LeaveCriticalSection(&new_listhead->lock);
        }
        unlock_pte_region(region);

        // TODO do we actually want to track this?
        stop_counter(&time_counter);
        DOUBLE duration = get_counter_duration(&time_counter);
        track_time(duration, trim_batch_size, trim_times,
                  &trim_time_index, TRIM_TIMES_TO_TRACK);

        return;
    }

    // Unmap the pages with the Windows API
    unmap_pages_scatter(virtual_addresses, trim_batch_size);

    EnterCriticalSection(&modified_page_list.lock);

    link_list_to_tail(&modified_page_list, &batch_list);

    LeaveCriticalSection(&modified_page_list.lock);

    // Iterate over each PFN we captured and change its state along with its PTE
    pfn = CONTAINING_RECORD(batch_list.entry.Flink, PFN, entry);
    for (ULONG i = 0; i < trim_batch_size; i++)
    {
        // Update the PFN to make it modified
        PFN pfn_contents = read_pfn(pfn);
        pfn_contents.flags.state = MODIFIED;
        write_pfn(pfn, pfn_contents);

        PPFN next_pfn = CONTAINING_RECORD(pfn->entry.Flink, PFN, entry);

        // Zero the valid bit and make the PTE a transition PTE
        current_pte = pte_from_va(virtual_addresses[i]);
        PTE pte_contents = read_pte(current_pte);
        pte_contents.entire_format = 0;
        pte_contents.transition_format.always_zero = 0;
        pte_contents.transition_format.frame_number = frame_number_from_pfn(pfn);
        pte_contents.transition_format.always_zero2 = 0;
        write_pte(current_pte, pte_contents);

        unlock_pfn(pfn);
        pfn = next_pfn;
    }

    for (ULONG i = 0; i < NUMBER_OF_AGES; i++)
    {
        WriteUShortNoFence(&region->age_count.ages[i], local_count.ages[i]);
    }

    // Find the oldest age to insert the region back into the age lists
    ULONG oldest_age = 0;
    for (LONG i = NUMBER_OF_AGES - 1; i >= 0; i--)
    {
        if (local_count.ages[i] != 0)
        {
            oldest_age = i;
            break;
        }
    }

    PPTE_REGION_LIST new_listhead = &pte_region_age_lists[oldest_age];

    EnterCriticalSection(&new_listhead->lock);
    add_region_to_list(region, new_listhead);
    LeaveCriticalSection(&new_listhead->lock);

    // Update the global age count
    for (ULONG i = 0; i < NUMBER_OF_AGES; i++)
    {
        InterlockedAdd64((volatile LONG64 *) &global_age_count.pages_of_age[i],
                         local_count.ages[i] - old_count.ages[i]);
    }

    unlock_pte_region(region);

    stop_counter(&time_counter);
    DOUBLE duration = get_counter_duration(&time_counter);
    track_time(duration, trim_batch_size, trim_times,
               &trim_time_index, TRIM_TIMES_TO_TRACK);

    // Decrease the target trims by the number of pages we trimmed
    if (*target_trims < trim_batch_size) {
        *target_trims = 0;
    }
    *target_trims -= trim_batch_size;
}

DWORD trimming_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    HANDLE handles[2];
    handles[0] = system_exit_event;
    handles[1] = trim_wake_event;

    WaitForSingleObject(system_start_event, INFINITE);
    // TODO status

    while (TRUE)
    {
        ULONG64 num_trims = 0;

        // Find how many pages are of each age and how many should be trimmed from each age to meet the target
        ULONG64 average_page_consumption = average_page_consumption_global;

        ULONG64 consumable_pages = *(volatile ULONG_PTR *) (&free_page_list.num_pages) +
                                         *(volatile ULONG_PTR *) (&standby_page_list.num_pages);

        DOUBLE time_until_no_pages = (DOUBLE) consumable_pages / (DOUBLE) average_page_consumption;

        // Calculate time required to trim and mod-write these pages
        TIME_MEASURE trim_average = average_tracked_times(trim_times, ARRAYSIZE(trim_times));
        TIME_MEASURE mw_average = *(volatile TIME_MEASURE *) &global_mw_average;

        DOUBLE trim_time = (DOUBLE) average_page_consumption * (trim_average.duration / (DOUBLE) trim_average.num_pages);
        DOUBLE mw_time = (DOUBLE) average_page_consumption * (mw_average.duration / (DOUBLE) mw_average.num_pages);
        DOUBLE time_to_trim_and_mw = trim_time + mw_time;

        // If we need to start trimming before pages run out
        if (time_until_no_pages <= time_to_trim_and_mw) {
            // Schedule trimming and mod-writing for pages_needed
            num_trims = average_page_consumption;
        } else {
            ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles,
                                             FALSE, INFINITE);
            if (index == 0)
            {
                // TODO Set status
                break;
            }
        }

        // Trim as many batches as the thread was told to
        while (num_trims > 0)
        {
            trim_pte_region(&num_trims);
        }
    }

    return 0;
}
