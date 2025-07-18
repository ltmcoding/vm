#ifndef SCHEDULER_H
#define SCHEDULER_H
#include <Windows.h>

#define MOD_WRITE_TIMES_TO_TRACK                     16
#define SECONDS_TO_TRACK                             16

typedef struct {
    ULONG64 duration;
    ULONG64 num_pages;
} MOD_WRITE_TIME, PMOD_WRITE_TIME;

extern MOD_WRITE_TIME mod_write_times[MOD_WRITE_TIMES_TO_TRACK];
extern ULONG64 mod_write_time_index;

extern ULONG64 available_pages[SECONDS_TO_TRACK];
extern ULONG64 available_pages_index;

extern ULONG64 num_batches_to_write;
extern ULONG64 num_pages_to_age;
extern ULONG64 num_pages_to_trim;

extern MOD_WRITE_TIME average_mod_write_times(VOID);
extern VOID track_mod_write_time(ULONG64 duration, ULONG64 num_pages);

extern ULONG64 average_page_consumption(VOID);
extern VOID track_available_pages(ULONG64 available_pages);

extern DWORD task_scheduling_thread(PVOID context);





#endif //SCHEDULER_H
