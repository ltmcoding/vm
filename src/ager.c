#include "../include/vm.h"

ULONG64 age_pte_region(PPTE_REGION *current_region)
{
    ULONG64 ptes_aged = 0;

    // Check if the current region is active
    PPTE_REGION region = *current_region;
    if (is_region_active(region))
    {
        lock_pte_region(region);
    }
    else {
        region = get_next_active_region(region);
        lock_pte_region(region);
    }

    // Because accessing the PTEs in this region will mess up the age count, we need to recount here
    PTE_REGION_AGE_COUNT local_count;

    // Grab the first PTE in the region
    PPTE current_pte = pte_from_pte_region(region);

    // Iterate over the entire region aging it
    for (ULONG64 index = 0; index < PTE_REGION_SIZE; index++)
    {
        if (current_pte == pte_end)
        {
            break;
        }

        if (current_pte->memory_format.valid == 1 && current_pte->memory_format.accessed == 1)
        {
            // If the PTE is accessed, we reset its age
            PTE local = read_pte(current_pte);
            local.memory_format.age = 0;
            write_pte(current_pte, local);
            increase_age_count(&local_count, 0);
            ptes_aged++;
        }
        else if (current_pte->memory_format.valid == 1)
        {
            // Increase the age count for the PTE's age
            ULONG age = current_pte->memory_format.age;
            increase_age_count(&local_count, age);

            // If the PTE is not the oldest age, we can age it
            if (current_pte->memory_format.age < NUMBER_OF_AGES - 1) {
                PTE local = read_pte(current_pte);
                local.memory_format.age++;
                write_pte(current_pte, local);
                ptes_aged++;
            }
        }
        current_pte++;
    }

    region->age_count = local_count;

    unlock_pte_region(region);

    // Increment the current region and check for wrap around
    *current_region++;
    if (*current_region >= pte_regions + NUMBER_OF_PTE_REGIONS) {
        *current_region = pte_regions;
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

    // Keep track of the last aged region so the ager can age circularly
    PPTE_REGION current_region = pte_regions;

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
            ULONG64 result = age_pte_region(&current_region);
            num_pages -= result;
        }
    }
    return 0;
}