#include "../include/vm.h"

// TODO take pte region locks
VOID age_pte_region(PPTE_REGION *current_region)
{
    ULONG64 ptes_aged = 0;
    TIME_COUNTER time_counter;
    start_counter(&time_counter);

    // Check if the current region is active
    PPTE_REGION region = *current_region;
    if (!is_region_active(region))
    {
        region = get_next_active_region(region);
    }

    // Because accessing the PTEs in this region will mess up the age count, we need to recount here
    PTE_REGION_AGE_COUNT local_count = {0};

    // Grab the first PTE in the region
    PPTE current_pte = pte_from_pte_region(region);

    // Iterate over the entire region aging it
    for (ULONG64 index = 0; index < PTE_REGION_SIZE; index++)
    {
        if (current_pte == pte_end)
        {
            break;
        }

        if (current_pte->memory_format.valid)
        {
            PTE local = read_pte(current_pte);
            ULONG age = local.memory_format.age;
            if (local.memory_format.accessed)
            {
                // If the PTE is accessed, we reset its age
                local.memory_format.age = 0;
                local.memory_format.accessed = 0;
                age = 0;
            }

            // Age the PTE if possible
            if (local.memory_format.age < NUMBER_OF_AGES - 1)
            {
                age++;
            }
            local.memory_format.age = age;

            BOOLEAN result = interlocked_write_pte(current_pte, local);
            if (!result)
            {
                // If the write failed, we skip this PTE
                // We do this because either the only way the PTE could be modified is if the accessed bit was set
                // In this case we don't want to age it so we don't need to increase the age count
                // Or the PTE was taken out of the active format and is now in transition or disc format
                current_pte++;
                continue;
            }

            increase_age_count(&local_count, age);
            ptes_aged++;
        }
        current_pte++;
    }

    region->age_count = local_count;

    // Increment the current region and check for wrap around
    *current_region++;
    if (*current_region >= pte_regions + NUMBER_OF_PTE_REGIONS) {
        *current_region = pte_regions;
    }

    stop_counter(&time_counter);
    DOUBLE duration = get_counter_duration(&time_counter);

    track_time(duration, ptes_aged, age_times,
               &age_time_index, AGE_TIMES_TO_TRACK);
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

        ULONG64 num_batches = *(volatile ULONG64 *) (&num_age_batches);
        for (ULONG64 i = 0; i < num_batches; i++)
        {
            age_pte_region(&current_region);
        }
    }
    return 0;
}