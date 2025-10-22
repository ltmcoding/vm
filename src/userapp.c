#include <stdio.h>
#include <Windows.h>
#include <stdlib.h>
#include "../include/userapp.h"

#include <system.h>

#include "../include/debug.h"

#pragma comment(lib, "advapi32.lib")

// This corresponds to how many times a random va will be written to in our test
// We are using MB(x) as a placeholder for 2^20, the purpose of this is just to get a large number
// This number represents the number of times we will access a random virtual address in our range
// MB has no reflection on the actual size of the memory we are using
#define NUM_PASSTHROUGHS  ((ULONG64) 2)

ULONG64 num_trims = 0;

VOID full_virtual_memory_test(VOID) {
    PULONG_PTR arbitrary_va;
    // ULONG random_number;
    BOOL page_faulted;
    PULONG_PTR pointer;
    ULONG_PTR num_bytes;
    ULONG_PTR local;

    ULONG_PTR virtual_address_size_in_pages;

    ULONG start_time;
    ULONG end_time;
    ULONG time_elapsed;

    ULONG thread_id;
    ULONG thread_index;

    // Compute faulting stats for the thread using thread index in the array
    thread_id = GetCurrentThreadId();
    thread_index = 0;
    for (ULONG i = 0; i < NUMBER_OF_FAULTING_THREADS; i++) {
        if (faulting_thread_ids[i] == thread_id) {
            thread_index = i;
            break;
        }
    }

    SHORT thread_line = (SHORT)(thread_index);
    PFAULT_STATS stats = &fault_stats[thread_index];

    // This replaces a malloc call in our system
    // Right now, we are just giving a set amount to the caller
    pointer = (PULONG_PTR) allocate_memory(&num_bytes);

    virtual_address_size_in_pages = num_bytes / PAGE_SIZE;

    //PULONG_PTR p_end = pointer + (virtual_address_size_in_pages * PAGE_SIZE) / sizeof(ULONG_PTR);

    ULONG64 slice_size = virtual_address_size_in_pages / NUMBER_OF_FAULTING_THREADS;
    slice_size = slice_size * PAGE_SIZE / sizeof(ULONG_PTR);

    ULONG64 slice_start = slice_size * thread_index;

    // This is where the test is actually ran
    start_time = GetTickCount();

    for (ULONG64 passthrough = 0; passthrough < NUM_PASSTHROUGHS; passthrough++) {

        // printf("full_virtual_memory_test : thread %lu accessing passthrough %llu\n "
        //        "Total amount of accesses: %llu\n", thread_index, passthrough,
        //        virtual_address_size_in_pages * (passthrough + 1));

        for (ULONG64 rep = 0; rep < virtual_address_size_in_pages; ++rep) {
            // If we have never accessed the surrounding page size (4K)
            // portion, the operating system will receive a page fault
            // from the CPU and proceed to obtain a physical page and
            // install a PTE to map it - thus connecting the end-to-end
            // virtual address translation.  Then the operating system
            // will tell the CPU to repeat the instruction that accessed
            // the virtual address, and this time, the CPU will see the
            // valid PTE and proceed to obtain the physical contents
            // (without faulting to the operating system again).

            // This computes a random virtual address within our range

            // Calculate arbitrary VA from rep
            ULONG64 offset = slice_start + (rep * PAGE_SIZE) / sizeof(ULONG_PTR);
            offset = offset % num_bytes;
            arbitrary_va = pointer + offset;


            if (rep % 10000 == 0) {
                double fraction = (double)rep / virtual_address_size_in_pages;
                print_bar(thread_index, fraction, passthrough, NUM_PASSTHROUGHS, stats->num_faults);
            }

            // Write the virtual address into each page
            page_faulted = FALSE;
            // Try to access the virtual address, continue entering the handler until the page fault is resolved

            // Call the API function to try accessing the virtual address
            // This function simulates the CPU's actions of calling the page fault handler,
            // stamping accessed bits, resetting ages, abd retrying if the fault wasn't resolved.
            access_va(arbitrary_va);
        }
    }

    // This gets the time elapsed in milliseconds
    end_time = GetTickCount();
    time_elapsed = end_time - start_time;

    // Final status update


    // Consolidated into one print statement
    printf("\nfull_virtual_memory_test : thread %lu finished accessing %llu passthroughs "
           "of the virtual address space (%llu addresses total) in %lu ms (%f s)\n"
           "full_virtual_memory_test : thread %lu took %llu faults and %llu fake faults\n"
           "full_virtual_memory_test : thread %lu took %llu first accesses and %llu reaccesses\n\n",
           thread_index, NUM_PASSTHROUGHS, NUM_PASSTHROUGHS * virtual_address_size_in_pages, time_elapsed, time_elapsed / 1000.0,
           thread_index, stats->num_faults, stats->num_fake_faults,
           thread_index, stats->num_first_accesses, stats->num_reaccesses);
}

// This function controls a faulting thread
DWORD faulting_thread(PVOID context)
{
    UNREFERENCED_PARAMETER(context);

    WaitForSingleObject(system_start_event, INFINITE);

    full_virtual_memory_test();

    return 0;
}