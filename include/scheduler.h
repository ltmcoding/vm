#ifndef SCHEDULER_H
#define SCHEDULER_H
#include <Windows.h>

#define MOD_WRITE_TIMES_TO_TRACK                     16
#define TRIM_TIMES_TO_TRACK                          16
#define AGE_TIMES_TO_TRACK                           16

#define SECONDS_OF_PAGE_CONSUMPTION_TO_TRACK                             16
#define WAKEUP_INTERVAL_IN_MS 1000

// This measures the time it takes for system threads to do work and the number of pages they work with
// This struct is currently used to track the per page time cost of
// Modified writing, aging, and trimming
typedef struct {
    DOUBLE duration;
    ULONG64 num_pages;
} TIME_MEASURE, PTIME_MEASURE;

typedef struct {
    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
} TIME_COUNTER, *PTIME_COUNTER;

typedef struct {
    ULONG64 pages_of_age[NUMBER_OF_AGES];
} GLOBAL_AGE_COUNT, *PGLOBAL_AGE_COUNT;

extern TIME_MEASURE mod_write_times[MOD_WRITE_TIMES_TO_TRACK];
extern ULONG64 mod_write_time_index;
extern TIME_MEASURE global_mw_average;

extern TIME_MEASURE age_times[MOD_WRITE_TIMES_TO_TRACK];
extern ULONG64 age_time_index;

extern TIME_MEASURE trim_times[MOD_WRITE_TIMES_TO_TRACK];
extern ULONG64 trim_time_index;

extern ULONG64 pages_consumed;

extern HANDLE age_wake_event;
extern HANDLE mw_wake_event;
extern HANDLE trim_wake_event;

extern ULONG64 num_mod_writes_global;
extern ULONG64 num_ages_global;

extern GLOBAL_AGE_COUNT global_age_count;

extern ULONG64 average_page_consumption_global;

extern VOID track_time(DOUBLE duration, ULONG64 num_pages, TIME_MEASURE *times,
    ULONG64 *index, ULONG64 times_to_track);
TIME_MEASURE average_tracked_times(TIME_MEASURE *times, USHORT array_size);

extern VOID start_counter(PTIME_COUNTER counter);
extern VOID stop_counter(PTIME_COUNTER counter);
extern DOUBLE get_counter_duration(PTIME_COUNTER counter);

extern DWORD task_scheduling_thread(PVOID context);

#endif //SCHEDULER_H
