#include <Windows.h>
#include "../include/vm.h"
#include "../include/scheduler.h"

#include <stdio.h>

#include "debug.h"

HANDLE age_wake_event;
HANDLE mw_wake_event;
HANDLE trim_wake_event;

ULONG64 num_mod_writes_global;
ULONG64 num_ages_global;
ULONG64 num_trims_global;

GLOBAL_AGE_COUNT global_age_count;

TIME_MEASURE mod_write_times[MOD_WRITE_TIMES_TO_TRACK];
ULONG64 mod_write_time_index;
TIME_MEASURE global_mw_average;

TIME_MEASURE age_times[MOD_WRITE_TIMES_TO_TRACK];
ULONG64 age_time_index;

TIME_MEASURE trim_times[MOD_WRITE_TIMES_TO_TRACK];
ULONG64 trim_time_index;

ULONG64 pages_consumed;
ULONG64 pages_consumed_history[SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK];
ULONG64 pages_consumed_history_index;

ULONG64 average_page_consumption_global;

VOID start_counter(PTIME_COUNTER counter)
{
    QueryPerformanceFrequency(&counter->frequency);
    QueryPerformanceCounter(&counter->start_time);
}

VOID stop_counter(PTIME_COUNTER counter)
{
    QueryPerformanceCounter(&counter->end_time);
}

DOUBLE get_counter_duration(PTIME_COUNTER counter)
{
    LARGE_INTEGER elapsed;
    elapsed.QuadPart = counter->end_time.QuadPart - counter->start_time.QuadPart;
    return (DOUBLE) elapsed.QuadPart / (DOUBLE) counter->frequency.QuadPart;
}

TIME_MEASURE average_tracked_times(TIME_MEASURE *times, USHORT array_size)
{
    TIME_MEASURE average;
    DOUBLE total_duration = 0;
    ULONG64 total_pages = 0;

    USHORT i;
    for (i = 0; i < array_size; i++)
    {
        if (times[i].num_pages == MAXULONG64)
        {
            break;
        }
        total_duration += times[i].duration;
        total_pages += times[i].num_pages;
    }

    average.duration = total_duration / i;
    average.num_pages = total_pages / i;

    return average;
}

VOID track_time(DOUBLE duration, ULONG64 num_pages, TIME_MEASURE *times, ULONG64 *index, ULONG64 times_to_track)
{
    times[*index].duration = duration;
    times[*index].num_pages = num_pages;
    *index = (*index + 1) % times_to_track;
}

VOID track_consumed_pages(ULONG64 prev_pages_consumed)
{
    pages_consumed_history[pages_consumed_history_index] = prev_pages_consumed;
    pages_consumed_history_index = (pages_consumed_history_index + 1) % SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK;
}

ULONG64 average_consumed_pages(VOID)
{
    ULONG64 i;
    ULONG64 total_consumed = 0;
    for (i = 0; i < SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK; i++) {
        if (pages_consumed_history[i] == MAXULONG64) {
            // If the value is uninitialized, break out of the loop
            break;
        }
        total_consumed += pages_consumed_history[i];
    }
    return total_consumed / i;
}

// This controls the thread that constantly writes pages to disc when prompted by other threads
// In the future this should use a fraction of the CPU if the system cannot give it a full core
DWORD task_scheduling_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    // This thread needs to be able to react to handles for waking as well as exiting
    HANDLE handles[1];

    handles[0] = system_exit_event;

    // Initialize to a value that indicates uninitialized
    for (ULONG i = 0; i < SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK; i++) {
        pages_consumed_history[i] = MAXULONG64;
    }

    // This waits for the system to start before doing anything
    WaitForSingleObject(system_start_event, INFINITE);
    // TODO LM FIX GIVE THIS THREAD ITS OWN STATUS LINE

    while (TRUE)
    {
        // Wait for 1 second always unless the system is exiting
        ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, WAKEUP_INTERVAL_IN_MS);
        if (index == 0)
        {
            // TODO needs its own status
            break;
        }

        // This count could be totally broken, as the counts of free and standby page counts are from different times
        // We can trust them both individually at that time but not together
        ULONG64 consumable_pages = *(volatile ULONG_PTR *) (&free_page_list.num_pages) +
                                 *(volatile ULONG_PTR *) (&standby_page_list.num_pages);
        ULONG64 prev_pages_consumed = *(volatile ULONG64 *) (&pages_consumed);

        // Reset the pages consumed count for the next second
        InterlockedExchange64((volatile LONG64 *) &pages_consumed, 0);

        // Track the current number of available pages
        track_consumed_pages(prev_pages_consumed);

        // This finds the average number of pages that have been consumed a second in the last 16 seconds
        // Convert to seconds using WAKEUP_INTERVAL_IN_MS
        ULONG64 average_page_consumption = average_consumed_pages();
        average_page_consumption /= WAKEUP_INTERVAL_IN_MS / 1000;

        average_page_consumption_global = *(volatile ULONG64 *) (&average_page_consumption);

        // Find how long until we have no more free or standby pages
        DOUBLE time_until_no_pages = (DOUBLE) consumable_pages / (DOUBLE) average_page_consumption;

        ULONG64 num_ages_local = 0;

        // Take a tally of how many PTEs are of each age
        GLOBAL_AGE_COUNT age_count_snapshot;
        for (ULONG i = 0; i < NUMBER_OF_AGES; i++) {
            age_count_snapshot.pages_of_age[i] = *(volatile ULONG64 *)(&global_age_count.pages_of_age[i]);
        }

        ULONG64 total_active_pages = 0;
        for (ULONG i = 0; i < NUMBER_OF_AGES; i++) {
            total_active_pages += age_count_snapshot.pages_of_age[i];
        }
        if (total_active_pages == 0) {
            // If there are no active pages, we can skip aging
            SetEvent(mw_wake_event);
            SetEvent(trim_wake_event);
            continue;
        }

        // TODO reads at the top available pages

        // TODO why is mod writer falling behind?
        // TODO why is the global count of page ages broken

        // Find out how long it takes to age a page
        TIME_MEASURE age_average = average_tracked_times(age_times, ARRAYSIZE(age_times));
        DOUBLE age_per_page_cost = (DOUBLE) age_average.duration / (DOUBLE) age_average.num_pages;

        // Find how long it will take to age all the pages we need to age
        ULONG64 total_num_pages_to_age = total_active_pages * NUMBER_OF_AGES;
        DOUBLE time_to_age_all = (DOUBLE) total_num_pages_to_age * age_per_page_cost;;
        ULONG64 max_possible_ages = (ULONG64) (1.0 / age_per_page_cost);

        // If we have enough time we want to calculate the fraction of each second that we should be aging
        // And multiply it by the max number of batches we can age in a second
        // Otherwise we will simply age the maximum number of batches we can age in a second
        if (time_until_no_pages >= time_to_age_all) {
            // We know at this point that we have extra time so we don't need to age constantly
            // We divide the time to age all pages by the time until we have no more pages
            // This gives us a fraction of the time that we should age
            DOUBLE fraction_used = (DOUBLE) time_to_age_all / (DOUBLE) time_until_no_pages;
            assert(fraction_used <= 1.0);
            num_ages_local = (ULONG64) ((DOUBLE) max_possible_ages * fraction_used);
        } else {
            // Otherwise, we can age constantly
            num_ages_local = max_possible_ages;
        }

        *(volatile ULONG64 *)(&num_ages_global) = num_ages_local;

        SetEvent(age_wake_event);

        // Wake the trimmer mod writer do their own checks
        SetEvent(mw_wake_event);
        SetEvent(trim_wake_event);
    }

    return 0;
}