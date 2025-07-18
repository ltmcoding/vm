#include <Windows.h>
#include <stdio.h>
#include "../include/vm.h"
#include "../include/debug.h"

ULONG trim_pte_region(ULONG min_age_to_trim) {
    PFN_LIST batch_list;
    ULONG trim_batch_size = 0;
    ULONG64 frame_numbers[PTE_REGION_SIZE];

    initialize_listhead(&batch_list);

    for (LONG64 age = NUMBER_OF_AGES - 1; age >= 0; age--) {
        PPTE_REGION_LIST listhead = &pte_region_age_lists[age];
        EnterCriticalSection(&listhead->lock);

        // Try to pop a region from the list. If we succesfully can it comes with the lock held
        PPTE_REGION region = pop_region_from_list(listhead);
        if (region == NULL) {
            LeaveCriticalSection(&listhead->lock);
            continue;
        }
        LeaveCriticalSection(&listhead->lock);

        // Grab the first PTE in the region
        PPTE current_pte = pte_from_pte_region(region);
        ULONG oldest_age = 0;
        PPFN pfn;

        for (ULONG index = 0; index < PTE_REGION_SIZE; index++) {
            // Break early if we reach the end of the PTEs
            if (current_pte == pte_end)
            {
                break;
            }
            // Check the valid bit of the PTE
            if (current_pte->memory_format.valid == 1) {
                // If the page is not old enough, we skip it after noting its age
                if (current_pte->memory_format.age < min_age_to_trim) {

                    if (current_pte->memory_format.age > oldest_age) {
                        oldest_age = current_pte->memory_format.age;
                    }
                }
                // If the page is older than the minimum age, we trim it
                else {
                    pfn = pfn_from_frame_number(current_pte->memory_format.frame_number);
                    lock_pfn(pfn);

                    // If the page is being referenced by the modified writer, we cannot trim it
                    if (pfn->flags.reference == 1) {
                        unlock_pfn(pfn);
                        current_pte++;
                        continue;
                    }

                    add_to_list_head(pfn, &batch_list);
                    frame_numbers[index] = current_pte->memory_format.frame_number;

                    PVOID user_va = va_from_pte(current_pte);
                    NULL_CHECK(user_va, "trim : could not get the va connected to the pte")

                    // The user VA is still mapped, we need to unmap it here to stop the user from changing it
                    // Any attempt to modify this va will lead to a page fault so that we will not be able to have stale data
                    unmap_pages(user_va, 1);

                    // This writes the new contents into the PTE and PFN
                    PTE old_pte_contents = read_pte(current_pte);
                    assert(old_pte_contents.memory_format.valid == 1)

                    PTE new_pte_contents;
                    // The PTE is zeroed out here to ensure no stale data remains
                    new_pte_contents.entire_format = 0;
                    new_pte_contents.transition_format.always_zero = 0;
                    new_pte_contents.transition_format.frame_number = old_pte_contents.memory_format.frame_number;
                    new_pte_contents.transition_format.always_zero2 = 0;

                    write_pte(current_pte, new_pte_contents);

                    PFN pfn_contents = read_pfn(pfn);
                    pfn_contents.flags.state = MODIFIED;
                    write_pfn(pfn, pfn_contents);
                }
            }
            current_pte++;
        }

        trim_batch_size = batch_list.num_pages;
        assert(batch_list.num_pages != 0);

        EnterCriticalSection(&modified_page_list.lock);

        link_list_to_tail(&modified_page_list, &batch_list);

        LeaveCriticalSection(&modified_page_list.lock);

        for (ULONG index = 0; index < PTE_REGION_SIZE; index++) {
            ULONG frame_number = frame_numbers[index];
            if (frame_number == 0)
            {
                continue;
            }
            unlock_pfn(pfn_from_frame_number(frame_number));
        }

        // Add the region back to the list of regions for the oldest age
        EnterCriticalSection(&pte_region_age_lists[oldest_age].lock);
        add_region_to_list(region, &pte_region_age_lists[oldest_age]);
        LeaveCriticalSection(&pte_region_age_lists[oldest_age].lock);

        unlock_pte(pte_from_pte_region(region));
        break;
    }

    return trim_batch_size;
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
        ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles,
                                             FALSE, INFINITE);
        if (index == 0)
        {
            // TODO Set status
            break;
        }

        LONG64 num_pages = *(volatile ULONG64 *) (&num_pages_to_trim);
        ULONG min_age_to_trim = *(volatile ULONG *) (&min_age_to_trim);

        while (num_pages > 0)
        {
            ULONG result = trim_pte_region(min_age_to_trim);
            num_pages -= result;
        }
    }

    return 0;
}
