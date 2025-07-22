#ifndef VM_USERAPP_H
#define VM_USERAPP_H
#include <stdio.h>
#include <Windows.h>
#include "hardware.h"

extern PULONG faulting_thread_ids;

extern ULONG64 num_trims;

extern PVOID allocate_memory(PULONG_PTR num_bytes);
extern VOID access_va(PULONG_PTR arbitrary_va);

extern DWORD faulting_thread(PVOID context);
#endif //VM_USERAPP_H
