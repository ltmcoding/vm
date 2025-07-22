#ifndef VM_SYSTEM_H
#define VM_SYSTEM_H
#include <Windows.h>

#define MAX_MOD_BATCH                   ((ULONG64) 256)

#define NULL_CHECK(x, msg)       if (x == NULL) {fatal_error(msg); }

extern ULONG_PTR physical_page_count;
extern ULONG_PTR virtual_address_size;

extern PVOID va_base;
extern PVOID va__end;

extern PVOID modified_write_va;
extern PVOID modified_read_va;
extern PVOID repurpose_zero_va;

extern CRITICAL_SECTION modified_write_va_lock;
extern CRITICAL_SECTION modified_read_va_lock;
extern CRITICAL_SECTION repurpose_zero_va_lock;

extern HANDLE wake_aging_event;
extern HANDLE modified_writing_event;
extern HANDLE pages_available_event;
extern HANDLE disc_spot_available_event;
extern HANDLE system_exit_event;
extern HANDLE system_start_event;

extern DWORD modified_write_thread(PVOID context);
extern DWORD trim_thread(PVOID context);

extern VOID initialize_system(VOID);
extern VOID run_system(VOID);
extern VOID deinitialize_system(VOID);

extern VOID fatal_error(char *msg);
extern VOID map_pages(PVOID user_va, ULONG_PTR page_count, PULONG_PTR page_array);
extern VOID unmap_pages(PVOID user_va, ULONG_PTR page_count);
extern VOID map_pages_scatter(PVOID *user_va, ULONG_PTR page_count, PULONG_PTR page_array);
extern VOID unmap_pages_scatter(PVOID *user_va, ULONG_PTR page_count);

#endif //VM_SYSTEM_H
