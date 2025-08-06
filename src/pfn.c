#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"

PPFN pfn_base;
PPFN pfn_end;
ULONG_PTR highest_frame_number = 0;
ULONG_PTR lowest_frame_number = MAXULONG_PTR;

PPFN pfn_from_frame_number(ULONG64 frame_number)
{
    NULL_CHECK((void *) frame_number, "pfn_from_frame_number : frame number is null")

    if (frame_number > highest_frame_number || frame_number <= 0)
    {
        fatal_error("pfn_from_frame_number : frame number is out of valid range");
    }

    // Again, the compiler implicitly multiplies frame number by PFN size
    return pfn_base + frame_number;
}

ULONG64 frame_number_from_pfn(PPFN pfn)
{
    NULL_CHECK(pfn, "frame_number_from_pfn : pfn is null")

    if (pfn > pfn_end || pfn < pfn_base)
    {
        fatal_error("frame_number_from_pfn : pfn is out of valid range");
    }

    return pfn - pfn_base;
}

// This strategy is not usable for PFNs because they are too large
PFN read_pfn(PPFN pfn)
{
    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), READ);
    #endif

    return *pfn;
}

VOID write_pfn(PPFN pfn, PFN local)
{
    pfn->pte = local.pte;
    pfn->flags = local.flags;
    pfn->disc_index = local.disc_index;

    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), WRITE);
    #endif
}

VOID lock_pfn(PPFN pfn)
{
    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), LOCK);
    #endif

    EnterCriticalSection(&pfn->lock);
}

VOID unlock_pfn(PPFN pfn)
{
    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), UNLOCK);
    #endif

    LeaveCriticalSection(&pfn->lock);
}

BOOLEAN try_lock_pfn(PPFN pfn)
{
    BOOLEAN result = TryEnterCriticalSection(&pfn->lock);

    #if READWRITE_LOGGING
    if (result == TRUE) {
        log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), TRY_SUCCESS);
    }
    else {
        log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), TRY_FAIL);
    }
    #endif

    return result;
}
