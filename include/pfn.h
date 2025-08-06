#ifndef VM_PFN_H
#define VM_PFN_H
#include <Windows.h>
#include "pte.h"

#define FREE 0
#define STANDBY 1
// The zeroed state is currently unimplemented
//#define ZEROED 2
#define MODIFIED 3
#define ACTIVE 4

typedef struct {
    // States are FREE, STANDBY, ZEROED (to be added), ACTIVE, and MODIFIED
    ULONG state:3;
    ULONG dirtied:1;
    ULONG reference:3;
}PFN_FLAGS/*, *PPFN_FLAGS*/;

typedef struct {
    LIST_ENTRY entry;
    PPTE pte;
    PFN_FLAGS flags;
    ULONG64 disc_index;
    // In the future, these locks will be made into single bits instead of massive CRITICAL_SECTIONS
    CRITICAL_SECTION lock;
} PFN, *PPFN;

extern PPFN pfn_base;
extern PPFN pfn_end;
extern ULONG_PTR highest_frame_number;
extern ULONG_PTR lowest_frame_number;

extern ULONG64 frame_number_from_pfn(PPFN pfn);
extern PPFN pfn_from_frame_number(ULONG64 frame_number);

extern VOID lock_pfn(PPFN pfn);
extern VOID unlock_pfn(PPFN pfn);
extern BOOLEAN try_lock_pfn(PPFN pfn);

extern PFN read_pfn(PPFN pfn);
extern VOID write_pfn(PPFN pfn, PFN pfn_contents);

#endif //VM_PFN_H
