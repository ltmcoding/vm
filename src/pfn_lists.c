#include <stdio.h>
#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"

PFN_LIST free_page_list;
PFN_LIST modified_page_list;
PFN_LIST standby_page_list;

VOID initialize_listhead(PPFN_LIST listhead)
{
    listhead->entry.Flink = listhead->entry.Blink = &listhead->entry;
    listhead->num_pages = 0;
}

BOOLEAN is_list_empty(PPFN_LIST listhead)
{
    ULONG64 empty_count = listhead->num_pages == 0;
    BOOLEAN empty_list = listhead->entry.Flink == &listhead->entry;
    assert(empty_count == empty_list);
    return empty_list;
}

// Removes the pfn from the list corresponding to its state
VOID remove_from_list(PPFN pfn)
{
    PPFN_LIST listhead;

    // Finds the list based on the page's state
    // There is no active state, and we never remove free pages in this way
    // So we know that this is either a modified or standby page
    if (pfn->flags.state == MODIFIED) {

        listhead = &modified_page_list;

    } else if (pfn->flags.state == STANDBY) {

        // This is on the standby list
        listhead = &standby_page_list;

    } else {
        fatal_error("remove_from_list : tried to remove a page from list when it was on none");
        return;
    }

    // Checks the integrity of the list before and after the remove operation
    check_list_integrity(listhead, pfn);

    // This removes the page from the list by erasing it from the chain of FLinks and blinks
    PLIST_ENTRY prev_entry = pfn->entry.Blink;
    PLIST_ENTRY next_entry = pfn->entry.Flink;

    prev_entry->Flink = next_entry;
    next_entry->Blink = prev_entry;

    pfn->entry.Flink = NULL;
    pfn->entry.Blink = NULL;

    listhead->num_pages--;

    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), REMOVE_MIDDLE);
    #endif

    check_list_integrity(listhead, NULL);
}

// This removes the first element from a pfn list and returns it
PPFN pop_from_list_head_helper(PPFN_LIST listhead)
{
    PPFN pfn;
    PLIST_ENTRY entry;
    PLIST_ENTRY next_entry;

    // Checks the integrity of the list before and after the remove operation
    check_list_integrity(listhead, NULL);

    // Removes from the head of the list
    entry = listhead->entry.Flink;
    next_entry = entry->Flink;
    listhead->entry.Flink = next_entry;
    next_entry->Blink = &listhead->entry;

    pfn = CONTAINING_RECORD(entry, PFN, entry);

    listhead->num_pages--;

    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), REMOVE_HEAD);
    #endif

    check_list_integrity(listhead, NULL);

    return pfn;
}

// Returns a locked page
PPFN pop_from_list_head(PPFN_LIST listhead)
{
    BOOLEAN took_page = FALSE;
    PPFN peeked_page;
    PPFN taken_page;

    EnterCriticalSection(&listhead->lock);

    while (took_page == FALSE) {
        // Peek at the head of the list
        // If the list is empty, return
        if (is_list_empty(listhead))
        {
            LeaveCriticalSection(&listhead->lock);
            return NULL;
        }

        // If not empty, grab the head of the list
        peeked_page = CONTAINING_RECORD(listhead->entry.Flink, PFN, entry);
        // Try to lock the pfn at the head
        if (try_lock_pfn(peeked_page) == FALSE) {
            // If we can't lock the pfn then we relinquish the lock on the list and try again
            LeaveCriticalSection(&listhead->lock);
            // We relinquish the lock in hopes that another thread
            // Will make progress on this page and relinquish its lock or take the page entirely
            // Try to do the operation again
            EnterCriticalSection(&listhead->lock);
            continue;
        }

        assert(listhead->num_pages != 0)
        // Do the actual work of removing it now that we know its safe
        taken_page = pop_from_list_head_helper(listhead);
        assert(taken_page == peeked_page)
        // Relinquish lock for list
        LeaveCriticalSection(&listhead->lock);
        took_page = TRUE;
    }

    return taken_page;
}

// Returns a locked page
PFN_LIST batch_pop_from_list_head(PPFN_LIST listhead, PPFN_LIST batch_list, ULONG64 batch_size, BOOLEAN reference_mode)
{
    PPFN peeked_page;
    PLIST_ENTRY flink_entry;

    initialize_listhead(batch_list);
    batch_list->num_pages = 0;

    flink_entry = listhead->entry.Flink;

    for (ULONG64 i = 0; i < batch_size; i++) {
        // We don't expect this to happen, but if it does, we just return the batch list
        if (flink_entry == &listhead->entry)
        {
            return *batch_list;
        }

        peeked_page = CONTAINING_RECORD(flink_entry, PFN, entry);
        // Save the next entry before trying to lock the page, as removing it from the list will clear the entry
        PLIST_ENTRY next_entry = flink_entry->Flink;
        // Try to lock the pfn at the head
        if (try_lock_pfn(peeked_page) == TRUE) {
            remove_from_list(peeked_page);
            add_to_list_tail(peeked_page, batch_list);

            // If the reference mode is on, we need to set the reference bit to 1 and then unlock the page
            if (reference_mode == TRUE) {
                peeked_page->flags.reference += 1;
                peeked_page->flags.dirtied = 0;
                unlock_pfn(peeked_page);
            }
        }
        // The next entry should be moved to no matter what, as we have moved on from the current page
        flink_entry = next_entry;
    }

    return *batch_list;
}

// This function simply adds a page to a specified list
VOID add_to_list_tail(PPFN pfn, PPFN_LIST listhead) {
    // Inserts it on the list, checking the integrity of it before and after
    check_list_integrity(listhead, NULL);

    PLIST_ENTRY last_entry = listhead->entry.Blink;
    pfn->entry.Flink = &listhead->entry;
    pfn->entry.Blink = last_entry;
    last_entry->Flink = &pfn->entry;
    listhead->entry.Blink = &pfn->entry;

    listhead->num_pages++;
    check_list_integrity(listhead, pfn);

    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), INSERT_TAIL);
    #endif
}

// This function is used to re-add a page to the head of a list
VOID add_to_list_head(PPFN pfn, PPFN_LIST listhead) {
    check_list_integrity(listhead, NULL);

    PLIST_ENTRY first_entry = listhead->entry.Flink;
    pfn->entry.Flink = first_entry;
    pfn->entry.Blink = &listhead->entry;
    first_entry->Blink = &pfn->entry;
    listhead->entry.Flink = &pfn->entry;

    listhead->num_pages++;
    check_list_integrity(listhead, pfn);


    #if READWRITE_LOGGING
    log_access(IS_A_FRAME_NUMBER, (PVOID) frame_number_from_pfn(pfn), INSERT_HEAD);
    #endif
}

// This functiion takes an existing PFN list and links it to the tail of another list
// It is called with all necessary locks held for PFNs and PFN_LISTS
// This destroys the last list
VOID link_list_to_tail(PPFN_LIST first, PPFN_LIST last) {
    PLIST_ENTRY first_blink = first->entry.Blink;
    PLIST_ENTRY last_flink = last->entry.Flink;
    PLIST_ENTRY last_blink = last->entry.Blink;

    first_blink->Flink = last_flink;
    last_flink->Blink = first_blink;
    last_blink->Flink = &first->entry;
    first->entry.Blink = last_blink;

    first->num_pages += last->num_pages;
}