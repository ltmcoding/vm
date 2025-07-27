#include "Windows.h"
#include "../include/vm.h"
#include "../include/debug.h"

PPTE pte_base;
PPTE pte_end;

PPTE_REGION pte_regions;
PPTE_REGION pte_regions_end;

PPTE_REGION_LIST pte_region_age_lists;


// These functions convert between matching linear structures (pte and va)
PPTE pte_from_va(PVOID virtual_address)
{
    // Null and out of bounds checks done for security purposes
    NULL_CHECK(virtual_address, "pte_from_va : virtual address is null")

    if ((ULONG_PTR) virtual_address > (ULONG_PTR) va_base + virtual_address_size
        || virtual_address < va_base)
    {
        fatal_error("pte_from_va : virtual address is out of valid range");
    }

    // We can compute the difference between the first va and our va
    // This will be equal to the difference between the first pte and the pte we want
    ULONG_PTR difference = (ULONG_PTR) virtual_address - (ULONG_PTR) va_base;
    difference /= PAGE_SIZE;

    // The compiler automatically multiplies the difference by the size of a pte, so it is not required here
    return pte_base + difference;
}

PVOID va_from_pte(PPTE pte)
{
    // Same checks done for security purposes
    NULL_CHECK(pte, "va_from_pte : pte is null")
    if (pte > pte_end || pte < pte_base)
    {
        fatal_error("va_from_pte : pte is out of valid range");
    }

    // The same math is done here but in reverse
    ULONG_PTR difference = (ULONG_PTR) (pte - pte_base);
    difference *= PAGE_SIZE;

    PVOID result = (PVOID) ((ULONG_PTR) va_base + difference);

    return result;
}

PPTE_REGION pte_region_from_pte(PPTE pte)
{
    NULL_CHECK(pte, "pte_region_from_pte : pte is null")
    if (pte < pte_base || pte >= pte_end)
    {
        fatal_error("pte_region_from_pte : pte is out of valid range");
    }

    ULONG64 index = (ULONG64) (pte - pte_base);
    index /= PTE_REGION_SIZE;

    return &pte_regions[index];
}

PPTE pte_from_pte_region(PPTE_REGION pte_region)
{
    NULL_CHECK(pte_region, "pte_from_pte_region : pte_region is null")
    if (pte_region < pte_regions || pte_region >= &pte_regions[NUMBER_OF_PTE_REGIONS])
    {
        fatal_error("pte_from_pte_region : pte_region is out of valid range");
    }

    ULONG64 index = (ULONG64) (pte_region - pte_regions);
    index *= PTE_REGION_SIZE;

    return &pte_base[index];
}

// These functions are used to read and write PTEs and PFNs in a way that doesn't conflict with other threads
PTE read_pte(PPTE pte)
{
    // This atomically reads the PTE as a single 64 bit value
    // This is needed because the CPU or another concurrent faulting thread
    // Can still access this PTE in transition format and see an intermediate state
    PTE local;
    local.entire_format = *(volatile ULONG64 *) &pte->entire_format;

#if READWRITE_LOGGING
    log_access(IS_A_PTE, pte, READ);
#endif

    return local;
}

// Write the value of a local PTE to a PTE in memory
VOID write_pte(PPTE pte, PTE local)
{
    // Now this is written as a single 64 bit value instead of in parts
    // This is needed because the cpu or another concurrent faulting thread
    // Can still access this pte in transition format and see an intermediate state
    *(volatile ULONG64 *) &pte->entire_format = local.entire_format;

#if READWRITE_LOGGING
    log_access(IS_A_PTE, pte, WRITE);
#endif
}

BOOLEAN interlocked_write_pte(PPTE pte, PTE local) {
    ULONG64 result = InterlockedCompareExchange64(
        (volatile LONG64 *) &pte->entire_format,
        local.entire_format,
        read_pte(pte).entire_format
    );

    if (result == read_pte(pte).entire_format) {
        return TRUE;
    }

    return FALSE;
}

// Functions to lock and unlock PTE regions
// This locks the entire region of PTEs that the PTE is in
VOID lock_pte(PPTE pte)
{
    // We do not need to cast or multiply/divide by the size of a pte
    // This is because the compiler is smart enough to do this for us
    ULONG64 index = pte - pte_base;
    index /= PTE_REGION_SIZE;

#if READWRITE_LOGGING
    log_access(IS_A_PTE, pte, LOCK);
#endif

    EnterCriticalSection(&pte_regions[index].lock);
}

VOID unlock_pte(PPTE pte)
{
    ULONG64 index = pte - pte_base;
    index /= PTE_REGION_SIZE;

#if READWRITE_LOGGING
    log_access(IS_A_PTE, pte, UNLOCK);
#endif

    LeaveCriticalSection(&pte_regions[index].lock);
}

BOOLEAN try_lock_pte(PPTE pte)
{
    ULONG64 index = pte - pte_base;
    index /= PTE_REGION_SIZE;

    BOOLEAN result = TryEnterCriticalSection(&pte_regions[index].lock);

#if READWRITE_LOGGING
    if (result == TRUE) {
        log_access(IS_A_PTE, pte, TRY_SUCCESS);
    }
    else {
        log_access(IS_A_PTE, pte, TRY_FAIL);
    }
#endif

    return result;
}

VOID initialize_region_listhead(PPTE_REGION_LIST listhead) {
    listhead->entry.Flink = listhead->entry.Blink = &listhead->entry;
}

VOID add_region_to_list(PPTE_REGION pte_region, PPTE_REGION_LIST listhead) {
    PLIST_ENTRY first_entry = listhead->entry.Flink;

    pte_region->entry.Flink = first_entry;
    pte_region->entry.Blink = &listhead->entry;

    first_entry->Blink = &pte_region->entry;

    listhead->entry.Flink = &pte_region->entry;

    listhead->num_regions++;
}

VOID remove_region_from_list(PPTE_REGION pte_region, PPTE_REGION_LIST listhead) {
    // Remove it from the list. it could be anywhere
    PLIST_ENTRY blink_entry = pte_region->entry.Blink;
    PLIST_ENTRY flink_entry = pte_region->entry.Flink;

    blink_entry->Flink = flink_entry;
    flink_entry->Blink = blink_entry;

    pte_region->entry.Blink = NULL;
    pte_region->entry.Flink = NULL;

    listhead->num_regions--;
}

PPTE_REGION pop_region_from_list(PPTE_REGION_LIST listhead) {
    // This is a helper function that pops a region from the list
    // It is used to pop the first region from the list
    if (listhead->num_regions == 0) {
        return NULL;
    }

    PLIST_ENTRY current_entry = listhead->entry.Flink;
    PPTE_REGION pte_region = CONTAINING_RECORD(current_entry, PTE_REGION, entry);

    while (current_entry != &listhead->entry) {
        // Try to lock the region, if we can, we break out
        // If we cannot, we continue to the next entry
        if (TryEnterCriticalSection(&pte_region->lock)) {
            break;
        }

        current_entry = current_entry->Flink;
        if (current_entry == &listhead->entry) {
            // If we reached the end of the list, we return NULL
            return NULL;
        }

        pte_region = CONTAINING_RECORD(current_entry, PTE_REGION, entry);
    }

    remove_region_from_list(pte_region, listhead);
    return pte_region;
}

BOOLEAN is_region_active(PPTE_REGION pte_region) {
    // This checks if the region is active
    return pte_region->active == 1;
}

VOID make_region_active(PPTE_REGION pte_region) {
    // This makes the region active
    pte_region->active = 1;
}

VOID make_region_inactive(PPTE_REGION pte_region) {
    // This makes the region inactive
    pte_region->active = 0;
}

VOID lock_pte_region(PPTE_REGION pte_region) {
    EnterCriticalSection(&pte_region->lock);
}

BOOLEAN try_lock_pte_region(PPTE_REGION pte_region) {
    return TryEnterCriticalSection(&pte_region->lock);
}

VOID unlock_pte_region(PPTE_REGION pte_region) {
    LeaveCriticalSection(&pte_region->lock);
}

VOID increase_age_count(PPTE_REGION_AGE_COUNT age_count, ULONG age) {
    // This increases the age count for the given age
    if (age >= NUMBER_OF_AGES) {
        fatal_error("increase_age_count : age is out of valid range");
    }
    age_count->ages[age]++;
}

// TODO replace this with a bitmap that uses interlocked
PPTE_REGION get_next_active_region(PPTE_REGION pte_region) {
    do {
        pte_region++;
    } while (pte_region->active == 0);

    return pte_region;
}
