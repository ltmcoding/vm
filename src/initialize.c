#include <stdio.h>
#include <stdlib.h>
#include <userapp.h>
#include <Windows.h>
#include "../include/vm.h"
#include "../include/debug.h"
#include "../include/console.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "onecore.lib")

int compare(const void * a, const void * b);

PULONG_PTR physical_page_numbers;

// These are handles to our faulting threads
PHANDLE faulting_handles;
PULONG faulting_thread_ids;
PFAULT_STATS fault_stats;

// These are handles to our system threads, our trimming/modified-writing threads
PHANDLE system_handles;
PULONG system_thread_ids;

HANDLE physical_page_handle;

// These are handles to our events, which are used to signal between threads
HANDLE pages_available_event;
HANDLE disc_spot_available_event;

HANDLE system_exit_event;
HANDLE system_start_event;

HANDLE shared_memory_section;

// These are the locks used in our system
CRITICAL_SECTION modified_write_va_lock;
CRITICAL_SECTION modified_read_va_lock;
CRITICAL_SECTION repurpose_zero_va_lock;

char pagefile_path[MAX_PATH];

// These are page file handles
//HANDLE page_file;
//HANDLE page_file_mapping;

// This is Windows-specific code to acquire a privilege.
VOID get_privilege(VOID)
{
    set_initialize_status("initialize_system", "getting privilege");

    struct {
        DWORD count;
        LUID_AND_ATTRIBUTES privilege[1];
    } info;

    HANDLE h_process;
    HANDLE token;
    BOOL result;

    // Open the token.
    h_process = GetCurrentProcess();

    result = OpenProcessToken(h_process, TOKEN_ADJUST_PRIVILEGES, &token);

    if (result == FALSE) {
        fatal_error("get_privilege : cannot open process token.");
    }

    // Enable the privilege.
    info.count = 1;
    info.privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    // Get the LUID.
    result = LookupPrivilegeValue(NULL,
                                  SE_LOCK_MEMORY_NAME,
                                  &(info.privilege[0].Luid));

    if (result == FALSE) {
        fatal_error("get_privilege : cannot get privilege");
    }

    // Adjust the privilege and check the result
    result = AdjustTokenPrivileges(token, FALSE, (PTOKEN_PRIVILEGES) &info,
                                   0, NULL, NULL);

    if (result == FALSE) {
        printf("get_privilege : cannot adjust token privileges %lu", GetLastError());
        fatal_error(NULL);
    }

    if (GetLastError() != ERROR_SUCCESS) {
        fatal_error("get_privilege : cannot enable the SE_LOCK_MEMORY_NAME privilege - check local policy");
    }

    CloseHandle(token);
}

HANDLE CreateSharedMemorySection(VOID) {
    HANDLE section;
    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Create an AWE section.  Later we deposit pages into it and/or
    // return them.
    //

    parameter.Type = MemSectionExtendedParameterUserPhysicalFlags;
    parameter.ULong = 0;

    section = CreateFileMapping2 (INVALID_HANDLE_VALUE,
                                  NULL,
                                  SECTION_MAP_READ | SECTION_MAP_WRITE,
                                  PAGE_READWRITE,
                                  SEC_RESERVE,
                                  0,
                                  NULL,
                                  &parameter,
                                  1);

    return section;
}

// This function is used to initialize all the locks used in the system
VOID initialize_locks(VOID)
{
    set_initialize_status("initialize_system", "setting up locks");
    INITIALIZE_LOCK(modified_write_va_lock);
    INITIALIZE_LOCK(modified_read_va_lock);
    INITIALIZE_LOCK(repurpose_zero_va_lock);

    INITIALIZE_LOCK(free_page_list.lock);
    INITIALIZE_LOCK(standby_page_list.lock);
    INITIALIZE_LOCK(modified_page_list.lock);
}

// This function is used to initialize all the events used in the system
VOID initialize_events(VOID)
{
    set_initialize_status("initialize_system", "setting up events");
    // Synchronization Events
    mw_wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    NULL_CHECK(mw_wake_event, "initialize_events : could not initialize modified_writing_event")

    age_wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    NULL_CHECK(age_wake_event, "initialize_events : could not initialize age_wake_event")

    trim_wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    NULL_CHECK(trim_wake_event, "initialize_events : could not initialize trim_wake_event")

    pages_available_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    NULL_CHECK(pages_available_event, "initialize_events : could not initialize pages_available_event")

    disc_spot_available_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    NULL_CHECK(disc_spot_available_event, "initialize_events : could not initialize disc_spot_available_event")

    // Notification Events
    system_exit_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    NULL_CHECK(system_exit_event, "initialize_events : could not initialize system_exit_event")

    system_start_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    NULL_CHECK(system_start_event, "initialize_events : could not initialize system_start_event")
}

// This function initializes all of our threads. Once they are initialized, they immediately start running.
// For this reason, this needs to be our last step of initialization
VOID initialize_threads(VOID)
{
    faulting_handles = (PHANDLE) malloc(NUMBER_OF_FAULTING_THREADS * sizeof(HANDLE));
    NULL_CHECK(faulting_handles, "initialize_threads : could not allocate memory for faulting_handles")

    faulting_thread_ids = (PULONG) malloc(NUMBER_OF_FAULTING_THREADS * sizeof(ULONG));
    NULL_CHECK(faulting_thread_ids, "initialize_threads : could not allocate memory for faulting_thread_ids")

    fault_stats = (PFAULT_STATS) malloc(NUMBER_OF_FAULTING_THREADS * sizeof(FAULT_STATS));
    NULL_CHECK(fault_stats, "initialize_threads : could not allocate memory for fault_stats")

    for (ULONG i = 0; i < NUMBER_OF_FAULTING_THREADS; i++)
    {
        faulting_handles[i] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)
        faulting_thread,(LPVOID) (ULONG_PTR) i, 0, &faulting_thread_ids[i]);
        NULL_CHECK(faulting_handles[i], "initialize_threads : could not initialize thread handle for faulting_thread")
    }

    // Initialize faulting thread stat counters
    for (ULONG i = 0; i < NUMBER_OF_FAULTING_THREADS; i++)
    {
        fault_stats[i].num_faults = 0;
        fault_stats[i].num_first_accesses = 0;
        fault_stats[i].num_reaccesses = 0;
        fault_stats[i].num_fake_faults = 0;
    }

    system_handles = (PHANDLE) malloc(NUMBER_OF_SYSTEM_THREADS * sizeof(HANDLE));
    NULL_CHECK(system_handles, "initialize_threads : could not allocate memory for system_handles")

    system_thread_ids = (PULONG) malloc(NUMBER_OF_SYSTEM_THREADS * sizeof(ULONG));
    NULL_CHECK(system_thread_ids, "initialize_threads : could not allocate memory for system_thread_ids")

    system_handles[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)
    trimming_thread,(LPVOID) (ULONG_PTR) 0, 0, &system_thread_ids[0]);
    NULL_CHECK(system_handles[0], "initialize_threads : could not initialize thread handle for trimming_thread")

    system_handles[1] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)
    modified_write_thread,(LPVOID) (ULONG_PTR) 1, 0, &system_thread_ids[1]);
    NULL_CHECK(system_handles[1], "initialize_threads : could not initialize thread handle for modified_write_thread")

    system_handles[2] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)
    task_scheduling_thread,(LPVOID) (ULONG_PTR) 2, 0, &system_thread_ids[2]);
    NULL_CHECK(system_handles[2], "initialize_threads : could not initialize thread handle for task_scheduling_thread")

    system_handles[3] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)
    aging_thread,(LPVOID) (ULONG_PTR) 3, 0, &system_thread_ids[3]);
    NULL_CHECK(system_handles[3], "initialize_threads : could not initialize thread handle for aging_thread");
}


VOID initialize_pagefile_path()
{
    char currentDirectory[MAX_PATH];

    // Get the current directory
    if(!GetCurrentDirectory(MAX_PATH, currentDirectory)) {
        printf("Failed to get current directory\n");
        return;
    }

    // Go back one directory
    char* lastBackslash = strrchr(currentDirectory, '\\');
    if (lastBackslash == NULL) {
        printf("Failed to find last backslash\n");
        return;
    }
    *lastBackslash = '\0';

    // Format the path to the pagefile
    snprintf(pagefile_path, MAX_PATH, "%s", PAGEFILE_ABSOLUTE_PATH);
}

VOID initialize_page_file() {
    set_initialize_status("initialize_system", "configuring page file");

    initialize_pagefile_path();

    LARGE_INTEGER size;
    size.QuadPart = PAGE_FILE_SIZE_IN_BYTES;

    pagefile_handle = CreateFileA(pagefile_path, GENERIC_READ | GENERIC_WRITE, 0,
                                  NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);

    if (pagefile_handle == INVALID_HANDLE_VALUE) {
        printf("pagefilePath: %s\n", pagefile_path);
        fatal_error("Failed to open pagefile\n");
    }

    // Set the file size
    if (!SetFilePointerEx(pagefile_handle, size, NULL, FILE_BEGIN) || !SetEndOfFile(pagefile_handle)) {
        fatal_error("Failed to set file size\n");
    }

    HANDLE hMapFile = CreateFileMapping(
        pagefile_handle,
        NULL,
        PAGE_READWRITE,
        size.HighPart,
        size.LowPart,
        NULL
    );

    if (hMapFile == NULL) {
        fatal_error("Failed to create file mapping\n");
    }

    page_file = MapViewOfFile(
        hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        PAGE_FILE_SIZE_IN_BYTES
    );

    if (page_file == NULL) {
        fatal_error("Failed to map view of file\n");
    }

    CloseHandle(hMapFile);
}

VOID initialize_page_file_bitmap(VOID)
{
    set_initialize_status("initialize_system", "creating page file bitmap");

    // Get the number of pages on the disc and calculate the size of the bitmap

    // Allocates the memory for the disc in use bitmap
    // This will remain in memory, as it is used to track which pages are in use and not part of the page file

    // Initialize the bitmap
    page_file_bitmap = malloc(BITMAP_SIZE_IN_BYTES);
    NULL_CHECK(page_file_bitmap, "malloc failed to allocate memory for the page file bitmap");
    memset(page_file_bitmap, 0, BITMAP_SIZE_IN_BYTES);
    page_file_bitmap_end = page_file_bitmap + BITMAP_SIZE_IN_BYTES / sizeof (*page_file_bitmap);

    free_disc_spot_count = BITMAP_SIZE_IN_BITS;

    // Initialize the freed spaces array
    freed_spaces = (PULONG64) malloc(MAX_FREED_SPACES_SIZE * sizeof(ULONG64));
    NULL_CHECK(freed_spaces, "malloc failed to allocate memory for the freed spaces array");
    freed_spaces_size = 0;

}

// This function initializes our page lists
VOID initialize_page_lists(VOID)
{
    set_initialize_status("initialize_system", "creating free, modified, and standby page lists");

    initialize_listhead(&free_page_list);
    free_page_list.num_pages = 0;

    initialize_listhead(&modified_page_list);
    modified_page_list.num_pages = 0;

    initialize_listhead(&standby_page_list);
    standby_page_list.num_pages = 0;

}

// This initializes va space for our system to map pages into
// This is used when we zero out a page
// Or when we move a page from the page file to memory and vice versa
VOID initialize_system_va_space(VOID)
{
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = shared_memory_section;

    set_initialize_status("initialize_system", "setting up system VAs");

    modified_write_va = VirtualAlloc2(NULL, NULL, PAGE_SIZE * MAX_MOD_BATCH, MEM_RESERVE | MEM_PHYSICAL,
                                    PAGE_READWRITE, &parameter, 1);
    NULL_CHECK(modified_write_va, "initialize_system_va_space : could not reserve memory for modified write va")

    modified_read_va = VirtualAlloc2(NULL, NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL,
                                    PAGE_READWRITE, &parameter, 1);
    NULL_CHECK(modified_read_va, "initialize_system_va_space : could not reserve memory for modified read va")

    repurpose_zero_va = VirtualAlloc2(NULL, NULL, PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL,
                                    PAGE_READWRITE, &parameter, 1);
    NULL_CHECK(repurpose_zero_va, "initialize_system_va_space : could not reserve memory for repurpose zero va")
}

// This function initializes our virtual address space
VOID initialize_user_va_space(VOID)
{
    set_initialize_status("initialize_system", "initializing user va space");

    // Reserve a user address space region using the Windows kernel AWE (address windowing extensions) APIs
    // This will let us connect physical pages of our choosing to any given virtual address within our allocated region
    // We deliberately make this much larger than physical memory to illustrate how we can manage the illusion.
    // We need to still have a large enough amount of physical memory in order to have any performance

    virtual_address_size = (physical_page_count + NUMBER_OF_USER_DISC_PAGES) * PAGE_SIZE;

    // Round down to a PAGE_SIZE boundary
    // Uses bit operations instead of modulus to do this quicker
    virtual_address_size &= ~(PAGE_SIZE - 1);

    // TODO make global
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = shared_memory_section;

    va_base = VirtualAlloc2(NULL, NULL, virtual_address_size, MEM_RESERVE | MEM_PHYSICAL,
                            PAGE_READWRITE, &parameter, 1);

    // va_base = VirtualAlloc(NULL, virtual_address_size,MEM_RESERVE | MEM_PHYSICAL,
    //                        PAGE_READWRITE);
    NULL_CHECK(va_base, "initialize_user_va_space : could not reserve memory for va space")

    //va__end = va_base + virtual_address_size;
}

VOID initialize_pte_regions(VOID) {
    // Initialize the locks for the PTE regions
    // Add PTE_REGION_SIZE - 1 to virtual_address_size to round up in case of an uneven division
    ULONG64 num_pte_regions = (virtual_address_size + PTE_REGION_SIZE - 1) / PTE_REGION_COVERAGE_IN_BYTES;
    pte_regions = malloc(num_pte_regions * sizeof(PTE_REGION));
    pte_regions_end = pte_regions + num_pte_regions;
    NULL_CHECK(pte_regions, "initialize_pte_regions : could not allocate memory for pte regions");
    memset(pte_regions, 0, num_pte_regions * sizeof(PTE_REGION));

    for (ULONG64 i = 0; i < num_pte_regions; i++) {
        // No need to update age counts as this only applies to active pages
        // Initialize the critical section for the PTE region
        InitializeCriticalSection(&pte_regions[i].lock);
    }

    pte_region_age_lists = malloc(NUMBER_OF_AGES * sizeof(PTE_REGION_LIST));
    NULL_CHECK(pte_region_age_lists, "initialize_pte_regions : could not allocate memory for pte region age lists");
    memset(pte_region_age_lists, 0, NUMBER_OF_AGES * sizeof(PTE_REGION_LIST));
    for (ULONG64 i = 0; i < NUMBER_OF_AGES; i++) {
        // Initialize the list head for the PTE region
        initialize_region_listhead(&pte_region_age_lists[i]);
        InitializeCriticalSection(&pte_region_age_lists[i].lock);
    }

    if (pte_regions == NULL || pte_region_age_lists == NULL) {
        printf("initialize_pte_regions : could not allocate memory for pte regions or age lists\n");
        DebugBreak();
    }
}

// This function initializes the PTEs that we use to map virtual addresses to physical pages
VOID initialize_pte_metadata(VOID)
{
    set_initialize_status("initialize_system", "creating PTEs");

    ULONG_PTR num_pte_bytes = virtual_address_size / PAGE_SIZE * sizeof(PTE);

    pte_base = malloc(num_pte_bytes);
    NULL_CHECK(pte_base, "initialize_pte_metadata : could not allocate memory for pte metadata")
    pte_end = pte_base + num_pte_bytes / sizeof(PTE);

    memset(pte_base, 0, num_pte_bytes);
}

VOID insert_tail_list(PLIST_ENTRY listhead, PLIST_ENTRY entry) {
    PLIST_ENTRY tail_entry = listhead->Blink;
    entry->Flink = listhead;
    entry->Blink = tail_entry;
    tail_entry->Flink = entry;
    listhead->Blink = entry;
}

#define PFN_SIZE_NOT_DIVISIBLE_BY_PAGE_SIZE ((sizeof(PFN) % PAGE_SIZE) != 0)


// This function initializes the PFNs that we use to track the state of physical pages
VOID initialize_pfn_metadata(VOID)
{
    set_initialize_status("initialize_system", "initializing PFNs");

    // We need the highest frame number in our page pool
    // Because the operating system gives us random pages from its pool instead of a sequential range
    ULONG64 num_pfn_bytes = (highest_frame_number + 1) * sizeof(PFN);

    // We reserve memory for the PFNs, but do not commit it
    // This is because we do not have all PFNs between our lowest and highest frame numbers in our page pool
    // For example, we could have frame numbers 2, 7, and 10, but not 3, 4, 5, 6, 8, or 9
    // We will only commit memory for the PFNs that we actually have in our page pool
    // While reserving memory for all of them in order to have a O(1) lookup time between a frame number and its pfn
    pfn_base = VirtualAlloc(NULL, num_pfn_bytes, MEM_RESERVE, PAGE_READWRITE);
    NULL_CHECK(pfn_base, "initialize_pfn_metadata : could not reserve memory for pfn metadata")

    // pfn_base -= lowest_frame_number * sizeof(PFN);
    pfn_end = pfn_base + num_pfn_bytes;

    PPFN pfn;
    ULONG_PTR frame_number;

    for (ULONG64 i = 0; i < physical_page_count; i++)
    {
        frame_number = physical_page_numbers[i];
        // In order to differentiate between our different PTE states, we use a frame number of 0 to indicate that
        // the PTE has never been accessed before
        if (frame_number == 0) {
            continue;
        }

        // Calculate the offset for the current frame number
        ULONG_PTR offset = frame_number * sizeof(PFN);

#ifdef PFN_SIZE_NOT_DIVISIBLE_BY_PAGE_SIZE
        // Check if the PFN stretches between two 4K virtual addresses
        if ((offset / PAGE_SIZE) != ((offset + sizeof(PFN) - 1) / PAGE_SIZE))
        {
            // Commit memory for the next page as well
            LPVOID result = VirtualAlloc((BYTE*)pfn_base + offset, sizeof(PFN) + PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE);
            //NULL_CHECK(result, "initialize_pfn_metadata : could not commit memory for pfn metadata")
            if (result == NULL) {
                DebugBreak();
            }
        }
        else
#endif
        {
            // Commit memory for the pfn inside our reserved chunk
            LPVOID result = VirtualAlloc((BYTE*) pfn_base + offset, sizeof(PFN), MEM_COMMIT, PAGE_READWRITE);
            NULL_CHECK(result, "initialize_pfn_metadata : could not commit memory for pfn metadata")
        }

        // Commits memory for the pfn inside our reserved chunk
        LPVOID result = VirtualAlloc(pfn_base + physical_page_numbers[i], sizeof(PFN),
                                     MEM_COMMIT, PAGE_READWRITE);

        // If we could not commit memory for the pfn, we need to exit
        NULL_CHECK(result, "initialize_pfn_metadata : could not commit memory for pfn metadata")

        // Initializes the pfn
        memset(pfn_base + physical_page_numbers[i], 0, sizeof(PFN));
        pfn = pfn_from_frame_number(frame_number);
        pfn->flags.state = FREE;
        // This should be done in initialize_locks, but this is an intermediary method of locking PFNs
        // So it is pointless to move
        INITIALIZE_LOCK(pfn->lock);

        // Inserts our newly initialized pfn into the free page list
        insert_tail_list(&free_page_list.entry, &pfn->entry);
        free_page_list.num_pages++;
    }
}

VOID find_frame_number_range(VOID) {
    // iterate through the physical page numbers to find the highest and lowest frame numbers

    for (ULONG64 i = 0; i < physical_page_count; i++) {
        if (physical_page_numbers[i] > highest_frame_number) {
            highest_frame_number = physical_page_numbers[i];
        }

        if (physical_page_numbers[i] < lowest_frame_number) {
            lowest_frame_number = physical_page_numbers[i];
        }
    }
}

// This function initializes our page pool
VOID initialize_pages()
{
    set_initialize_status("initialize_system", "Requesting physical pages from the system");

    physical_page_count = DESIRED_NUMBER_OF_PHYSICAL_PAGES;

    // We use this array to store all the frame numbers of every page we take
    physical_page_numbers = (PULONG_PTR) malloc(physical_page_count * sizeof(ULONG_PTR));
    NULL_CHECK(physical_page_numbers, "initialize_pages : could not allocate memory for physical page numbers")

    // This is where we actually allocate the pages of memory into our page pool
    if (AllocateUserPhysicalPages(shared_memory_section,&physical_page_count,
                                  physical_page_numbers) == FALSE) {
        fatal_error("initialize_pages : could not allocate physical pages");
    }

    if (physical_page_count != DESIRED_NUMBER_OF_PHYSICAL_PAGES) {
        printf("initialize_pages : allocated only %llu pages out of %llu pages requested", physical_page_count,
               DESIRED_NUMBER_OF_PHYSICAL_PAGES);
    }


    find_frame_number_range();
}

VOID initialize_time_measures(VOID) {
    for (ULONG i = 0; i < MOD_WRITE_TIMES_TO_TRACK; i++) {
        mod_write_times[i].num_pages = MAXULONG64;
    }
    for (ULONG i = 0; i < AGE_TIMES_TO_TRACK; i++) {
        age_times[i].num_pages = MAXULONG64;
    }
    for (ULONG i = 0; i < TRIM_TIMES_TO_TRACK; i++) {
        trim_times[i].num_pages = MAXULONG64;
    }

    // Fill the first entries of the time measures with sample data
    track_time(0.010, MAX_MOD_BATCH, mod_write_times,
               &mod_write_time_index, MOD_WRITE_TIMES_TO_TRACK);

    track_time(0.001, PTE_REGION_SIZE, trim_times,
               &trim_time_index, TRIM_TIMES_TO_TRACK);

    track_time(0.001, PTE_REGION_SIZE, age_times,
               &age_time_index, AGE_TIMES_TO_TRACK);
}

void initialize_console(void) {
    InitializeCriticalSection(&console_lock);

    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

    system("cls");

    DWORD mode;
    GetConsoleMode(console, &mode);
    SetConsoleMode(console,
        mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
            | DISABLE_NEWLINE_AUTO_RETURN
            | ENABLE_PROCESSED_OUTPUT);

    set_initialize_status("initialize_system", "initializing console");
}

VOID run_system(VOID)
{
    // This sets the event to start the system
    SetEvent(system_start_event);

    // This waits for the tests to finish running before exiting the function
    // Our controlling thread will wait for this function to finish before exiting the test and reporting stats
    WaitForMultipleObjects(NUMBER_OF_FAULTING_THREADS, faulting_handles, TRUE, INFINITE);
}

// This function fully initializes our system
VOID initialize_system (VOID) {
    initialize_console();

    // Acquire privilege as the operating system reserves the sole right to allocate pages.
    get_privilege();

    //physical_page_handle = GetCurrentProcess();
    shared_memory_section = CreateSharedMemorySection();
    NULL_CHECK(shared_memory_section, "initialize_system : could not create shared memory section");

    initialize_locks();

    initialize_events();

    initialize_pages();

    initialize_page_lists();

    initialize_pfn_metadata();

    initialize_page_file();

    initialize_page_file_bitmap();

    initialize_user_va_space();

    initialize_system_va_space();

    initialize_pte_metadata();

    initialize_pte_regions();

    initialize_time_measures();

    set_initialize_status("initialize_system", "system successfully initialized, running tests");

    initialize_threads();
}

VOID delete_pagefile() {
    UnmapViewOfFile(page_file);
    CloseHandle(pagefile_handle);
    DeleteFileA(pagefile_path);
}

// Terminates the program and gives all resources back to the operating system
VOID deinitialize_system (VOID)
{
    set_initialize_status("deinitialize_system", "Tests finished, deinitializing system");

    // We need to close all system threads and wait for them to exit before proceeding
    // This happens so that no thread tries to access a data structure that we have freed
    SetEvent(system_exit_event);
    WaitForMultipleObjects(NUMBER_OF_FAULTING_THREADS, system_handles, TRUE, INFINITE);

    // Now that we're done with our memory, we are able to free it
    free(pte_base);
    VirtualFree(modified_read_va, PAGE_SIZE, MEM_RELEASE);
    VirtualFree(modified_write_va, PAGE_SIZE, MEM_RELEASE);
    VirtualFree(va_base, virtual_address_size, MEM_RELEASE);
    free(page_file_bitmap);
    delete_pagefile();

    VirtualFree(pfn_base, physical_page_numbers[physical_page_count - 1] * sizeof(PFN),
                MEM_RELEASE);

    // We can also do the same for all of our pages
    FreeUserPhysicalPages(physical_page_handle,&physical_page_count,
                          physical_page_numbers);

    // Free the thread handles and thread id arrays
    free(faulting_handles);
    free(faulting_thread_ids);
    free(fault_stats);
    free(system_handles);
    free(system_thread_ids);

    set_initialize_status("deinitialize_system", "tests finished, system successfully deinitialized");
}