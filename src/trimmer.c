#include <Windows.h>
#include <stdio.h>
#include "../include/vm.h"
#include "../include/debug.h"

// TODO what if there are no age 8s left so we exit early but there are age 7s
ULONG trim_pte_region(PULONG pages_to_trim) {
    PFN_LIST batch_list;
    ULONG trim_batch_size = 0;
    PTE_REGION_AGE_COUNT local_count;
    PVOID virtual_addresses[PTE_REGION_SIZE];
    PPFN pfn;
    PPTE_REGION region;

    TIME_COUNTER time_counter;
    start_counter(&time_counter);

    // Try to find a region to trim
    LONG age = NUMBER_OF_AGES - 1;
    while (age >= 0) {
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
        return trim_batch_size;
    }


    initialize_listhead(&batch_list);

    // Grab the first PTE in the region
    PPTE current_pte = pte_from_pte_region(region);
    for (ULONG index = 0; index < PTE_REGION_SIZE; index++) {
        // Break early if we reach the end of the PTEs
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
                write_pte(current_pte, local);
                increase_age_count(&local_count, 0);
                current_pte++;
                continue;
            }
            // If the thread is allowed to trim this PTE, we will do so
            if (pages_to_trim[current_pte->memory_format.age] > 0) {
                pfn = pfn_from_frame_number(current_pte->memory_format.frame_number);
                lock_pfn(pfn);

                // If the page is being referenced by the modified writer, we cannot trim it
                if (pfn->flags.reference == 1) {
                    unlock_pfn(pfn);
                    // We want to still count the PTE for the age that it is in
                    increase_age_count(&local_count, current_pte->memory_format.age);
                    current_pte++;
                    continue;
                }

                // Add it to the list of pages to unmap and trim
                add_to_list_head(pfn, &batch_list);
                virtual_addresses[trim_batch_size] = va_from_pte(current_pte);
                NULL_CHECK(virtual_addresses[trim_batch_size], "trim : could not get the va connected to the pte")
                trim_batch_size++;

            }
            // Otherwise, it stays active, and the thread needs to recount its age for the PTE region
            else {
                increase_age_count(&local_count, current_pte->memory_format.age);
            }
        }
        current_pte++;
    }

    assert(trim_batch_size = batch_list.num_pages)
    // If we did not trim any pages, we can skip the rest of the processing and break out
    // We only expect this to happen in edge cases where the ages of every trimmable page in the region
    // Are reset by memory accesses
    if (trim_batch_size == 0) {
        region->age_count = local_count;
        // Find the oldest age to insert the region back into the age lists
        // Set oldest age to a value that is larger than any possible age
        // If it stays that way, it means that no active pages remain in the region
        ULONG oldest_age = NUMBER_OF_AGES + 1;
        for (ULONG i = 0; i < NUMBER_OF_AGES; i++) {
            if (local_count.ages[i] != 0) {
                oldest_age = i;
            }
        }

        // If there are no active pages left in the region, we can make it inactive, unlock, and break
        if (oldest_age == NUMBER_OF_AGES + 1) {
            make_region_inactive(region);
            unlock_pte_region(region);
            return trim_batch_size;
        }

        PPTE_REGION_LIST new_listhead = &pte_region_age_lists[oldest_age];

        EnterCriticalSection(&new_listhead->lock);
        add_region_to_list(region, new_listhead);
        LeaveCriticalSection(&new_listhead->lock);

        unlock_pte_region(region);

        stop_counter(&time_counter);
        DOUBLE duration = get_counter_duration(&time_counter);
        track_time(duration, trim_batch_size, trim_times,
                  &trim_time_index, TRIM_TIMES_TO_TRACK);

        return trim_batch_size;
    }

    // Unmap the pages with the Windows API
    unmap_pages_scatter(virtual_addresses, trim_batch_size);

    EnterCriticalSection(&modified_page_list.lock);

    link_list_to_tail(&modified_page_list, &batch_list);

    LeaveCriticalSection(&modified_page_list.lock);

    // Iterate over each PFN we captured and change its state along with its PTE
    PLIST_ENTRY current_entry = batch_list.entry.Flink;
    while (current_entry != &batch_list.entry)
    {
        pfn = CONTAINING_RECORD(current_entry, PFN, entry);
        current_pte = pfn->pte;

        // Zero the valid bit and make the PTE a transition PTE
        PTE pte_contents = read_pte(current_pte);
        pte_contents.entire_format = 0;
        pte_contents.transition_format.always_zero = 0;
        pte_contents.transition_format.frame_number = frame_number_from_pfn(pfn);
        pte_contents.transition_format.always_zero2 = 0;
        write_pte(current_pte, pte_contents);

        // Update the PFN to make it modified
        PFN pfn_contents = read_pfn(pfn);
        pfn_contents.flags.state = MODIFIED;
        write_pfn(pfn, pfn_contents);

        // Unlock the PFN
        unlock_pfn(pfn);

        current_entry = current_entry->Flink;
    }

    region->age_count = local_count;
    // Find the oldest age to insert the region back into the age lists
    ULONG oldest_age = 0;
    for (ULONG i = 0; i < NUMBER_OF_AGES; i++) {
        if (local_count.ages[i] != 0) {
            oldest_age = i;
        }
    }
    // If the
    PPTE_REGION_LIST new_listhead = &pte_region_age_lists[oldest_age];

    EnterCriticalSection(&new_listhead->lock);
    add_region_to_list(region, new_listhead);
    LeaveCriticalSection(&new_listhead->lock);

    unlock_pte_region(region);

    return trim_batch_size;
}

DWORD trimming_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    HANDLE handles[2];
    handles[0] = system_exit_event;
    handles[1] = trim_wake_event;

    PULONG local_num_pages_to_trim = malloc(NUMBER_OF_AGES * sizeof(ULONG64));
    WaitForSingleObject(system_start_event, INFINITE);
    // TODO status

    while (TRUE)
    {
        ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles,
                                             FALSE, INFINITE);
        if (index == 0)
        {
            // TODO Set status
            free(local_num_pages_to_trim);
            break;
        }

        // Get the target number of pages to trim for each age and copy it to a local array
        PULONG64 remote_list = (volatile PULONG64) ages_to_trim;
        memcpy(local_num_pages_to_trim, remote_list, NUMBER_OF_AGES * sizeof(ULONG64));

        ULONG64 local_batches = *(volatile ULONG64 *) (&num_trim_batches);

        // Trim as many batches as the thread was told to
        for (ULONG64 i = 0; i < local_batches; i++) {
            trim_pte_region(local_num_pages_to_trim);
        }
    }

    return 0;
}
