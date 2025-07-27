#include <Windows.h>
#include "../include/vm.h"
#include "../include/scheduler.h"

#include <stdio.h>

#include "debug.h"

HANDLE age_wake_event;
HANDLE mw_wake_event;
HANDLE trim_wake_event;

ULONG64 num_mw_batches;
ULONG64 num_age_batches;
ULONG64 num_trim_batches;
ULONG64 ages_to_trim[NUMBER_OF_AGES];

TIME_MEASURE mod_write_times[MOD_WRITE_TIMES_TO_TRACK];
ULONG64 mod_write_time_index;

TIME_MEASURE age_times[MOD_WRITE_TIMES_TO_TRACK];
ULONG64 age_time_index;

TIME_MEASURE trim_times[MOD_WRITE_TIMES_TO_TRACK];
ULONG64 trim_time_index;

ULONG64 available_pages[SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK];
ULONG64 available_pages_index;

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

TIME_MEASURE average_tracked_times(TIME_MEASURE *times, ULONG64 array_size)
{
    TIME_MEASURE average;
    DOUBLE total_duration = 0;
    ULONG64 total_pages = 0;

    USHORT i;
    for (i = 0; i < array_size; i++)
    {
        if (times[i].num_pages == 0)
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


ULONG64 average_page_consumption(VOID) {
    ULONG64 total = 0;

    ULONG64 count = 0;
    for (count = 0; count < ARRAYSIZE(available_pages); count++) {
        if (available_pages[count] != MAXULONG64) {
            total += available_pages[count];
            count++;
        }
        else {
            break;
        }
    }

    ULONG64 result = total / count;
    if (result == 0) {
        return 1;
    }
    return result;
}

VOID track_available_pages(ULONG64 num_pages)
{
    // Compute the previous index
    ULONG64 previous_index;
    if (available_pages_index == 0) {
        previous_index = SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK - 1;
    }
    else {
        previous_index = available_pages_index - 1;
    }

    // If the previous index is uninitialized, we know that this is the first time we are tracking this
    // So we set the available pages to the total number of pages - the number of pages we are tracking
    if (available_pages[previous_index] == MAXULONG64) {
        available_pages[available_pages_index] = physical_page_count - num_pages;
    }
    else {
        available_pages[available_pages_index] = available_pages[previous_index] - num_pages;
    }

    available_pages_index = (available_pages_index + 1) % SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK;
}

// This function reads without locks, as it is only used for statistics and taking locks would kill the peerformance
// The margin of error of this function is very low, as it reads a large quantity of regions
ULONG64 tally_pte_ages(PULONG64 output) {
    PPTE_REGION current_region = pte_regions;
    ULONG64 total_active_pages = 0;
    while (current_region != pte_regions_end) {
        // Add the age counts to the output
        for (ULONG i = 0; i < NUMBER_OF_AGES; i++) {
            output[i] += current_region->age_count.ages[i];
            total_active_pages += current_region->age_count.ages[i];
        }

        current_region++;
    }
    return total_active_pages;
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
        available_pages[i] = MAXULONG64;
    }

    // This waits for the system to start before doing anything
    WaitForSingleObject(system_start_event, INFINITE);
    // TODO LM FIX GIVE THIS THREAD ITS OWN STATUS LINE

    while (TRUE)
    {
        // Wait for 1 second always unless the system is exiting
        ULONG index = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 1000);
        if (index == 0)
        {
            // TODO needs its own status
            break;
        }

        // This count could be totally broken, as the counts of free and standby page counts are from different times
        // We can trust them both individually at that time but not together
        ULONG64 consumable_pages = *(volatile ULONG_PTR *) (&free_page_list.num_pages) +
                                 *(volatile ULONG_PTR *) (&standby_page_list.num_pages);

        // Track the current number of available pages
        track_available_pages(consumable_pages);

        // This finds the average number of pages that have been consumed a second in the last 16 seconds
        ULONG64 average_pages_consumed = average_page_consumption();

        // Find how long until we have no more free or standby pages
        DOUBLE time_until_no_pages = (DOUBLE) consumable_pages / (DOUBLE) average_pages_consumed;

        // Take a tally of how many PTEs are of each age
        ULONG64 ages_tally[NUMBER_OF_AGES];
        ULONG64 total_active_pages = tally_pte_ages(ages_tally);

        // Find how long it takes to write a page to disc
        TIME_MEASURE mw_average = average_tracked_times(mod_write_times, ARRAYSIZE(mod_write_times));
        DOUBLE mw_per_page_cost = mw_average.duration / (DOUBLE) mw_average.num_pages;

        // Find how long it will take us to empty our modified list completely and convert to seconds
        ULONG64 modified_pages = modified_page_list.num_pages;
        DOUBLE time_to_empty_modified = (DOUBLE) modified_pages * mw_per_page_cost;

        // Find the maximum amount of modified pages we can write in a second
        ULONG64 max_possible_mw_batches = (ULONG64) (1.0 / mw_per_page_cost / (DOUBLE) mw_average.num_pages);
        ULONG64 num_mw_batches_local = 0;

        // If we don't have enough time to empty the modified list, write constantly
        if (time_until_no_pages <= (ULONG64) time_to_empty_modified && time_to_empty_modified != 0) {
            num_mw_batches_local = max_possible_mw_batches;
        }
        else {
            // We know at this point that we have extra time so we don't need to write constantly
            // We divide the time to empty the modified list by the time until we have no more pages
            // This gives us a fraction of the time that we should write
            DOUBLE fraction_used = (DOUBLE) time_to_empty_modified / (DOUBLE) time_until_no_pages;
            assert(fraction_used <= 1);
            num_mw_batches_local = (ULONG64) ((DOUBLE) max_possible_mw_batches * fraction_used);
        }

        // Find how many pages are of each age and how many should be trimmed from each age to meet the target
        ULONG64 pages_needed = average_pages_consumed;

        // Calculate time required to trim and mod-write these pages
        TIME_MEASURE trim_average = average_tracked_times(trim_times, ARRAYSIZE(trim_times));

        DOUBLE trim_time = (DOUBLE)pages_needed * (trim_average.duration / (DOUBLE)trim_average.num_pages);
        DOUBLE mw_time = (DOUBLE)pages_needed * (mw_average.duration / (DOUBLE)mw_average.num_pages);
        DOUBLE total_time_needed = trim_time + mw_time;

        ULONG64 num_trim_batches_local = 0;
        // If we need to start trimming before pages run out
        if ((DOUBLE)time_until_no_pages <= total_time_needed) {
            // Schedule trimming and mod-writing for pages_needed
            num_trim_batches_local = pages_needed / trim_average.num_pages;
            if (num_trim_batches_local == 0) num_trim_batches_local = 1;

        } else {
            // Otherwise, no need to trim yet
            num_trim_batches_local = 0;
        }

        ULONG64 ages_to_trim_local[NUMBER_OF_AGES];

        for (LONG i = NUMBER_OF_AGES; i >= 0; i--)
        {
            // If we have no pages to trim, we can break out
            if (pages_needed <= 0) {
                break;
            }

            // If there are a surplus of pages in the age, we just trim the number of pages we need and break
            if (ages_tally[i] > pages_needed) {
                ages_to_trim_local[i] = pages_needed;
                break;
            }

            // Otherwise, we trim all the pages in the age that we can
            if (ages_tally[i] > 0) {
                ages_to_trim_local[i] = ages_tally[i];
                pages_needed -= ages_tally[i];
            }
        }

        ULONG64 num_age_batches_local = 0;

        // Find out how long it takes to age a page
        TIME_MEASURE age_average = average_tracked_times(age_times, ARRAYSIZE(age_times));
        DOUBLE age_per_page_cost = (DOUBLE) age_average.duration / (DOUBLE) age_average.num_pages;

        // Find how long it will take to age all the pages we need to age
        ULONG64 total_num_pages_to_age = total_active_pages * NUMBER_OF_AGES;
        DOUBLE time_to_age_all = (DOUBLE) total_num_pages_to_age * age_per_page_cost;;
        ULONG64 max_possible_age_batches = (ULONG64) (1.0 / age_per_page_cost / (DOUBLE) age_average.num_pages);

        // If we have enough time we want to calculate the fraction of each second that we should be aging
        // And multiply it by the max number of batches we can age in a second
        // Otherwise we will simply age the maximum number of batches we can age in a second
        if (time_until_no_pages <= time_to_age_all) {
            // We know at this point that we have extra time so we don't need to age constantly
            // We divide the time to age all pages by the time until we have no more pages
            // This gives us a fraction of the time that we should age
            DOUBLE fraction_used = (DOUBLE) time_to_age_all / (DOUBLE) time_until_no_pages;
            assert(fraction_used <= 1);
            num_age_batches_local = (ULONG64) ((DOUBLE) max_possible_age_batches * fraction_used);
        } else {
            // Otherwise, we can age constantly
            num_age_batches_local = max_possible_age_batches;
        }

        num_mw_batches = num_mw_batches_local;
        num_age_batches = num_age_batches_local;
        num_trim_batches = num_trim_batches_local;
        memcpy(ages_to_trim, ages_to_trim_local, NUMBER_OF_AGES * sizeof(ULONG64));

        SetEvent(mw_wake_event);
        SetEvent(age_wake_event);
        SetEvent(trim_wake_event);
    }

    return 0;
}