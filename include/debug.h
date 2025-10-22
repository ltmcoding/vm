#ifndef VM_DEBUG_H
#define VM_DEBUG_H
#include <Windows.h>
#include "pfn_lists.h"
#include "pfn.h"
#include "console.h"

// Creates a central switch to turn debug mode on/off
#define DBG                1

#if DBG
#define assert(x)       if (!(x)) { printf ("Assertion failed: %s, file %s, line %d\n", #x, __FILE__, __LINE__); DebugBreak(); }
#else
#define assert(x)
#endif

#define SPIN_COUNTS                              0

#if SPIN_COUNTS
#define SPIN_COUNT                               0xFFFFFF
#define INITIALIZE_LOCK(x)                       InitializeCriticalSectionAndSpinCount(&x, SPIN_COUNT)
#else
#define INITIALIZE_LOCK(x)                       InitializeCriticalSection(&x)
#endif

#define VA_ACCESS_MARKING                            0

#define READWRITE_LOGGING                            0
#if READWRITE_LOGGING

#define LOG_SIZE                                 32768

#define IS_A_FRAME_NUMBER                        0
#define IS_A_PTE                                 1

#define READ                                     0
#define WRITE                                    1
#define LOCK                                     2
#define TRY_SUCCESS                              3
#define TRY_FAIL                                 4
#define UNLOCK                                   5
#define INSERT_HEAD                              6
#define INSERT_TAIL                              7
#define REMOVE_MIDDLE                            8
#define REMOVE_HEAD                              9
#define REMOVE_TAIL                              10

typedef struct {
    ULONG entry_index;
    PPTE pte_ptr;
    PTE pte_val;
    PVOID virtual_address;
    PPFN pfn_ptr;
    PFN pfn_val;
    ULONG64 frame_number;
    ULONG operation:4;
    PVOID stack_trace[8];
    ULONG64 accessing_thread_id;
} READWRITE_LOG_ENTRY;

extern READWRITE_LOG_ENTRY page_log[LOG_SIZE];
extern LONG64 readwrite_log_index;
#endif

typedef struct {
    ULONG64 num_first_accesses;
    ULONG64 num_reaccesses;
    ULONG64 num_faults;
    ULONG64 num_fake_faults;
} FAULT_STATS, *PFAULT_STATS;

extern PFAULT_STATS fault_stats;

extern VOID check_list_integrity(PPFN_LIST listhead, PPFN match_pfn);
extern VOID log_access(ULONG is_pte, PVOID ppte_or_fn, ULONG operation);
extern VOID print_va_access_rate(VOID);

#endif //VM_DEBUG_H
