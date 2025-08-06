#include "../include/vm.h"

ULONG64 age_pte_region(PPTE_REGION *current_region)
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

    lock_pte_region(region);
    // Find the list its in from the age count
    ULONG region_age = NUMBER_OF_AGES + 1;
    for (ULONG i = 0; i < NUMBER_OF_AGES; i++)
    {
        if (region->age_count.ages[i] != 0)
        {
            region_age = i;
        }
    }
    if (region_age == NUMBER_OF_AGES + 1)
    {
        // If the region was just made inactive, we can skip it
        unlock_pte_region(region);
        return 0;
    }

    // Find the list head for the region's age
    PPTE_REGION_LIST listhead = &pte_region_age_lists[region_age];

    // Because accessing the PTEs in this region will mess up the age count, we need to recount
    PTE_REGION_AGE_COUNT old_count = *(volatile PTE_REGION_AGE_COUNT *) &region->age_count;
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

        PTE local = read_pte(current_pte);
        if (local.memory_format.valid)
        {
            ULONG age = local.memory_format.age;
            if (local.memory_format.accessed)
            {
                // If the PTE is accessed, we reset its age
                local.memory_format.age = 0;
                local.memory_format.accessed = 0;
                age = 0;
            }
            // Age the PTE if possible, but not if it has just had its accessed bit reset
            else if (local.memory_format.age < NUMBER_OF_AGES - 1)
            {
                age++;
            }
            local.memory_format.age = age;

            write_pte(current_pte, local);
            increase_age_count(&local_count, age);
            ptes_aged++;
        }
        current_pte++;
    }

    for (ULONG i = 0; i < NUMBER_OF_AGES; i++)
    {
        WriteUShortNoFence(&region->age_count.ages[i], local_count.ages[i]);
    }

    // Use the old and new counts to update the global age count
    for (ULONG i = 0; i < NUMBER_OF_AGES; i++)
    {
        InterlockedAdd64((volatile LONG64 *) &global_age_count.pages_of_age[i],
                         local_count.ages[i] - old_count.ages[i]);
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
    if (new_listhead != listhead)
    {
        EnterCriticalSection(&listhead->lock);
        remove_region_from_list(region, listhead);
        LeaveCriticalSection(&listhead->lock);

        EnterCriticalSection(&pte_region_age_lists[oldest_age].lock);
        add_region_to_list(region, &pte_region_age_lists[oldest_age]);
        LeaveCriticalSection(&pte_region_age_lists[oldest_age].lock);
    }

    unlock_pte_region(region);

    // Increment the current region and check for wrap around
    *current_region++;
    if (*current_region >= pte_regions + NUMBER_OF_PTE_REGIONS) {
        *current_region = pte_regions;
    }

    stop_counter(&time_counter);
    DOUBLE duration = get_counter_duration(&time_counter);

    track_time(duration, ptes_aged, age_times,
               &age_time_index, AGE_TIMES_TO_TRACK);

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

        ULONG64 num_pte_ages = *(volatile ULONG64 *) (&num_ages_global);
        while (num_pte_ages > 0)
        {
            // Age the PTE region
            ULONG64 ptes_aged = age_pte_region(&current_region);
            if (ptes_aged >= num_pte_ages)
            {
               break;
            }
            num_pte_ages -= ptes_aged;
        }
    }
    return 0;
}