#include <Windows.h>
#include <stdio.h>
#include "../include/vm.h"
#include "../include/debug.h"

PPFN get_free_page(VOID);
PPFN read_page_on_disc(PPTE pte, PPFN free_page);

ULONG_PTR virtual_address_size;
ULONG_PTR physical_page_count;
PVOID va_base;
PVOID va__end;
PVOID modified_write_va;
PVOID modified_read_va;
PVOID repurpose_zero_va;

// This breaks into the debugger if possible,
// Otherwise it crashes the program
// This is only done if our state machine is irreparably broken (or attacked)
VOID fatal_error(char *msg)
{
    if (msg == NULL) {
        msg = "system unexpectedly terminated";
    }
    print_fatal_error(msg);

    DebugBreak();
    TerminateProcess(GetCurrentProcess(), 1);
}

VOID map_pages(PVOID virtual_address, ULONG_PTR num_pages, PULONG_PTR page_array)
{
    if (MapUserPhysicalPages(virtual_address, num_pages, page_array) == FALSE) {
        printf("map_pages : could not map VA %p to page %llX\n", virtual_address, page_array[0]);
        fatal_error(NULL);
    }
}

VOID unmap_pages(PVOID virtual_address, ULONG_PTR num_pages)
{
    if (MapUserPhysicalPages(virtual_address, num_pages, NULL) == FALSE) {
        printf("unmap_pages : could not unmap VA %p to page %llX\n", virtual_address, num_pages);
        fatal_error(NULL);
    }
}

VOID map_pages_scatter(PVOID *virtual_addresses, ULONG_PTR num_pages, PULONG_PTR page_array)
{
    if (MapUserPhysicalPagesScatter(virtual_addresses, num_pages, page_array) == FALSE) {
        printf("map_pages_scatter : could not map VA %p to page %llX\n", virtual_addresses[0], page_array[0]);
        fatal_error(NULL);
    }
}

VOID unmap_pages_scatter(PVOID *virtual_addresses, ULONG_PTR num_pages)
{
    if (MapUserPhysicalPagesScatter(virtual_addresses, num_pages, NULL) == FALSE) {
        printf("unmap_pages_scatter : could not unmap VA %p to page %llX\n", virtual_addresses[0], num_pages);
        fatal_error(NULL);
    }
}

// This is how we get pages for new virtual addresses as well as old ones only exist on the paging file
PPFN get_free_page(VOID) {
    PPFN free_page = NULL;

    // First, we check the free page list for pages
    // There is no value to checking if the list is empty
    // An attempt to pop from an empty list will return NULL, and we will move on to the next list
    // Once we allow users to free memory, we will need to zero this too and do so using a thread
    free_page = pop_from_list_head(&free_page_list);
    assert(free_page == NULL || free_page->flags.state == FREE)

    // This is where we take pages from the standby list and reallocate their physical pages for our new va to use
    if (free_page == NULL)
    {
        free_page = pop_from_list_head(&standby_page_list);
        if (free_page == NULL) {
            return NULL;
        }

        PPTE other_pte = free_page->pte;
        ULONG64 other_disc_index = free_page->disc_index;
        // We want to start writing entire PTEs instead of writing them bit by bit

        // We hold this PTEs PFN lock which allows us to access this PTE here without a lock
        PTE local = *other_pte;
        if (local.disc_format.always_zero == 1)
        {
            printf("Valid bit was never zeroed in PTE %p\n", other_pte);
            DebugBreak();
        }
        local.disc_format.on_disc = 1;
        local.disc_format.disc_index = other_disc_index;
        write_pte(other_pte, local);

        // This is where we clear the previous contents off of the repurposed page
        // This is important as it can corrupt the new user's data if not entirely overwritten,
        // It also would allow a program to see another program's memory (HUGE SECURITY VIOLATION)
        ULONG_PTR frame_number = frame_number_from_pfn(free_page);

        EnterCriticalSection(&repurpose_zero_va_lock);

        map_pages(repurpose_zero_va, 1, &frame_number);

        memset(repurpose_zero_va, 0, PAGE_SIZE);

        // Unmap the page from our va space
        unmap_pages(repurpose_zero_va, 1);

        LeaveCriticalSection(&repurpose_zero_va_lock);
    }

    // This is a last resort option when there are no available pages
    // We wake the aging thread and send this information to the fault handler
    // Which then waits on a page to become available
    // Aging is done here no matter what
    // Once we have depleted a page from the free/standby list, it is a good idea to consider aging
    return free_page;
}

// This reads a page from the paging file and writes it back to memory
PPFN read_page_on_disc(PPTE pte, PPFN free_page)
{
    // We don't need a pfn lock here because this page is not on a list
    // And therefore is not visible to any other threads
    ULONG_PTR frame_number = frame_number_from_pfn(free_page);

    EnterCriticalSection(&modified_read_va_lock);

    // We map these pages into our own va space to write contents into them, and then put them back in user va space
    map_pages(modified_read_va, 1, &frame_number);

    // This would be a disc driver that does this read and write in a real operating system
    //PVOID source = (PVOID) ((ULONG_PTR) page_file + (pte->disc_format.disc_index * PAGE_SIZE));
    //memcpy(modified_read_va, source, PAGE_SIZE);
    read_from_pagefile(pte->disc_format.disc_index, modified_read_va);

    unmap_pages(modified_read_va, 1);

    LeaveCriticalSection(&modified_read_va_lock);

    // Set the bit at disc_index in disc in use to be 0 to reuse the disc spot
    free_disc_index(pte->disc_format.disc_index);
    return free_page;
}

// Stamp the accessed bit in the corresponding PTE when a VA is accessed
// In real life, this would be done by the CPU automatically
// However my program needs to simulate it
VOID cpu_stamp(PVOID arbitrary_va) {
    PPTE pte = pte_from_va(arbitrary_va);
    NULL_CHECK(pte, "cpu_stamp : could not get pte from va")

    // If the page is valid, we update its age using interlocked
    BOOLEAN success = FALSE;
    PTE old_pte_contents;
    PTE new_contents;
    while (!success) {
        old_pte_contents = read_pte(pte);
        // If the page is not valid, we cannot update its age
        if (old_pte_contents.memory_format.valid == 0) {
            return;
        }
        // If the accessed bit is already set and the age is 0, we do not need to update it
        if (old_pte_contents.memory_format.accessed == 1 && old_pte_contents.memory_format.age == 0) {
            return;
        }
        new_contents = old_pte_contents;
        new_contents.memory_format.accessed = 1;
        new_contents.memory_format.age = 0;

        // Try to write with an interlocked compare exchange
        success = InterlockedCompareExchange64((volatile LONG64 *) &pte->entire_format,
            new_contents.entire_format, old_pte_contents.entire_format) == old_pte_contents.entire_format;
    }
}

// This is where we handle any access or fault of a page
VOID page_fault_handler(PVOID arbitrary_va)
{
    PPTE pte;
    PTE pte_contents;
    PPFN pfn;
    PFN pfn_contents;
    ULONG64 frame_number;

    // Pages go through the handler regardless of whether they have faulted or not
    // This is because even if a page is accessed without a fault, it's age in the pte must be updated

    // First, we need to get the actual pte corresponding to the va we faulted on
    pte = pte_from_va(arbitrary_va);
    NULL_CHECK(pte, "page_fault_handler : could not get pte from va")

    // This order of operations is very important
    // A pte lock MUST sequentially come before a pfn lock
    // This is because we must lock the pte corresponding to a faulted va in order to handle its fault
    // At this point we do not know the pfn and cannot find it without a pte lock
    lock_pte(pte);
    pte_contents = read_pte(pte);

    // This is where the age is updated on an active page that has not actually faulted
    // We refer to this as a fake fault
    // We know this page is active because its valid bit is set, which only exists in a memory format pte
    if (pte_contents.memory_format.valid == 1)
    {
        return;
    }

    // If the entre pte is zeroed, it means that this is a brand new va that has never been accessed.
    // Technically, we only need to know that on_disc and the field at the position of
    // Frame_number/disc_index are zero, but this is easier to check and more easily understood
    // We know now that we need to get a free/standby page and map it to this va
    if (pte_contents.entire_format == 0)
    {
        // Get_free_page now returns a locked page, so we do not need to do it here
        pfn = get_free_page();

        // This occurs when we get_free_page fails to find us a free page
        // When this happens, we release our lock on this pte and wait for pages to become available
        // Once we are able to map a page to this va, we return, which lets the thread fault on this va again
        if (pfn == NULL) {
            unlock_pte(pte);
            WaitForSingleObject(pages_available_event, INFINITE);
            return;
        }
    }
    // At this point, we know that this pte is in transition or disc format, as the valid bit is clear
    // We can now safely check for the on_disc bit, if it is set it signifies to us that our pte is in disc format
    // It has been trimmed, written to disc, and its physical page has been reused
    // We need to read this page from the paging file and remap it to the va
    // We call this a hard fault, as we have to read from disc in order to handle it
    // We want to minimize hard faults, as they takes exponentially longer than other types of faults to resolve
    else if (pte_contents.disc_format.on_disc == 1) {

        pfn = get_free_page();
        if (pfn == NULL) {
            unlock_pte(pte);
            WaitForSingleObject(pages_available_event, INFINITE);
            return;
        }

        // This is where we actually read the page from the disc and write its contents to our new page
        read_page_on_disc(pte, pfn);

        // At this point, we know that our pte is in transition format, as it is not active or on disc
        // This va must have been trimmed, but its pfn has not been repurposed
        // All we need to do is remove it from the standby or modified lists now
        // This is called a soft fault, as this can be resolved all in memory and doesn't require disc reads/writes
        // This is our ideal type of fault, as it takes the shortest amount of time to fix
    } else {
        // This will unlink our page from the standby or modified list
        // It uses the PFNs information to determine which list it is on

        pfn = pfn_from_frame_number(pte_contents.transition_format.frame_number);

        // Because we need to acquire a pfn before a PTE inside our get_free_page function,
        // We can no longer trust a transition format PTE's contents until we also lock its pfn
        // After locking the pfn we need to reread the pte and ensure that it did not become disc format
        lock_pfn(pfn);

        pte_contents = read_pte(pte);
        if (pte_contents.disc_format.on_disc == 1)
        {
            unlock_pfn(pfn);
            unlock_pte(pte);
            return;
        }

        // assert(pte_contents.transition_format.always_zero == 0)
        // assert(pte_contents.transition_format.always_zero2 == 0)
        // assert(pte_contents.transition_format.frame_number == frame_number_from_pfn(pfn))
        // assert(pfn->flags.state == STANDBY || pfn->flags.state == MODIFIED)

        if (pfn->flags.state == MODIFIED) {
            EnterCriticalSection(&modified_page_list.lock);
            remove_from_list(pfn);
            LeaveCriticalSection(&modified_page_list.lock);

        } else /*(pfn->flags.state == STANDBY) */{

            EnterCriticalSection(&standby_page_list.lock);
            remove_from_list(pfn);
            // Freeing the space here and updating the pfn lower down
            free_disc_index(pfn->disc_index);
            LeaveCriticalSection(&standby_page_list.lock);
        }
    }

    // We now have the page we need, we just need to correctly map it now to the pte and return
    pte_contents = read_pte(pte);
    pfn_contents = read_pfn(pfn);
    frame_number = frame_number_from_pfn(pfn);

    pte_contents.memory_format.frame_number = frame_number_from_pfn(pfn);
    pte_contents.memory_format.valid = 1;
    pte_contents.memory_format.age = 0;
    write_pte(pte, pte_contents);

    pfn_contents.pte = pte;
    pfn_contents.flags.state = ACTIVE;
    pfn_contents.disc_index = 0;

    if (pfn->flags.state == MODIFIED) {
        pfn_contents.flags.modified = 1;
    } else {
        pfn_contents.flags.modified = 0;
    }
    write_pfn(pfn, pfn_contents);

    // Update the PTE region
    PPTE_REGION pte_region = pte_region_from_pte(pte);

    // No matter what case we are in, the region has gained an extra active page so we increment the age count
    if (!is_region_active(pte_region)) {
        make_region_active(pte_region);
        EnterCriticalSection(&pte_region_age_lists[0].lock);
        add_region_to_list(pte_region, &pte_region_age_lists[0]);
        LeaveCriticalSection(&pte_region_age_lists[0].lock);
    }

    // Increment the age count for the region. We know that the age is 0
    pte_region->age_count.ages[0]++;

    // This is a Windows API call that confirms the changes we made with the OS
    // We have already mapped this va to this page on our side, but the OS also needs to do the same on its side
    // This is necessary as this is a user mode program
    map_pages(arbitrary_va, 1, &frame_number);

    unlock_pfn(pfn);
    unlock_pte(pte);
}

// Eventually, we will move this to an api.c and api.h file
// That way programs can use our memory manager without knowing or having to know anything about how it works
PVOID allocate_memory(PULONG_PTR num_bytes)
{
    *num_bytes = virtual_address_size;
    return va_base;
}

// Accesses a virtual address and checks its checksum
// Enters the page fault handler if a page fault occurs and simulates the CPU stamping the accessed bit
VOID access_va(PULONG_PTR arbitrary_va) {
    BOOLEAN page_faulted = FALSE;
    ULONG_PTR local;

    do {
        __try
        {
            // Here we read the value of the page contents associated with the VA
            local = *arbitrary_va;
            // Here we need to simulate our CPU stamping the page's accessed bit
            cpu_stamp(arbitrary_va);

            // This causes an error if the local value is not the same as the VA
            // This means that we mixed up page contents between different VAs
            if (local != 0) {
                if (local != (ULONG_PTR) arbitrary_va) {
                    fatal_error("full_virtual_memory_test : page contents are not the same as the VA");
                }
            } else {
                // We are trying to write the VA as a number into the page contents associated with that VA
                *arbitrary_va = (ULONG_PTR) arbitrary_va;
            }

            page_faulted = FALSE;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {
            page_faulted = TRUE;
            page_fault_handler(arbitrary_va);
        }

    } while (page_faulted == TRUE);
}

// This main is likely to be moved to userapp.c in the future
// Figure out attribute unused
int main (int argc, char** argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
     /* This is where we initialize and test our virtual memory management state machine

     We control the entirety of virtual and physical memory management with only two exceptions
        We ask the operating system for all physical pages we use to store data
        (AllocateUserPhysicalPages)
        We ask the operating system to connect one of our virtual addresses to one of our physical pages
        (MapUserPhysicalPages)

     In a real kernel program, we would do these things ourselves,
     But the operating system (for security reasons) does not allow us to.

     But we do everything else commonly features in a memory manager
     Including: maintaining translation tables, PTE and PFN data structures, management of physical pages,
     Virtual memory operations like handling page faults, materializing mappings, freeing them, trimming them,
     Writing them out to a paging file, bringing them back from the paging file, protecting them, and much more */

    initialize_system();

    run_system();

    deinitialize_system();

    return 0;
}