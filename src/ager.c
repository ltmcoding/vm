#include "../include/vm.h"

ULONG64 age_pte_regions(VOID)
{
    ULONG64 ptes_aged = 0;

    // Trim one region of each age
    for (ULONG age = 0; age < NUMBER_OF_AGES; age++) {
        // Get the list of PTE regions for the current age
        PPTE_REGION_LIST listhead = &pte_region_age_lists[age];
        EnterCriticalSection(&listhead->lock);

        PPTE_REGION region = pop_region_from_list(listhead);
        if (region == NULL) {
            LeaveCriticalSection(&listhead->lock);
            continue;
        }
        LeaveCriticalSection(&listhead->lock);

        // Grab the first PTE in the region
        PPTE current_pte = pte_from_pte_region(region);

        // Iterate over the entire region aging it
        for (ULONG64 index = 0; index < PTE_REGION_SIZE; index++)
        {
            if (current_pte == pte_end)
            {
                break;
            }

            if (current_pte->memory_format.valid == 1)
            {
                // If the page is already at the maximum age, we trim it
                if (current_pte->memory_format.age < NUMBER_OF_AGES - 1) {
                    PTE local = read_pte(current_pte);
                    local.memory_format.age++;
                    write_pte(current_pte, local);
                    ptes_aged++;
                }
            }
            current_pte++;
        }

        LeaveCriticalSection(&region->lock);
        end_of_region_loop:;
    }

    return ptes_aged;
}

DWORD aging_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    HANDLE handles[2];
    handles[0] = system_exit_event;
    handles[1] = age_wake_event;

    WaitForSingleObject(system_start_event, INFINITE);
    // TODO add a status for this thread

    while (TRUE)
    {
        ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles,
                                             FALSE, 1000);
        if (index == 0)
        {
            // TODO set status
            break;
        }

        LONG64 num_pages = *(volatile ULONG64 *) (&num_pages_to_age);
        while (num_pages > 0) {
            ULONG64 result = age_pte_regions();
            num_pages -= result;
        }
    }
    return 0;
}