#ifndef VM_PTE_H
#define VM_PTE_H
#include <Windows.h>

// This number is strategically chosen to be 512, as it corresponds to the number of entries in a page table
#define PTE_REGION_SIZE                          (ULONG64) 512
#define PTE_REGION_COVERAGE_IN_BYTES             (PTE_REGION_SIZE * sizeof(PTE))

#define NUMBER_OF_AGES                           (ULONG64) 8
#define BITS_PER_AGE                             (ULONG64) 3

// With a region size of 512, we have 2MB of virtual memory per region
#define NUMBER_OF_PTE_REGIONS                    ((DESIRED_NUMBER_OF_PHYSICAL_PAGES + NUMBER_OF_USER_DISC_PAGES) / PTE_REGION_SIZE)

// We know that a PTE is in valid format if the valid bit is set
typedef struct {
    ULONG64 valid:1;
    ULONG64 accessed:1;
    ULONG64 frame_number:40;
    ULONG64 age:BITS_PER_AGE;
} VALID_PTE /*, *PVALID_PTE*/;

// We know that a PTE is in disc format if the valid bit is not set and on_disc is set
typedef struct {
    ULONG64 always_zero:1;
    ULONG64 disc_index:40;
    ULONG64 on_disc:1;
} INVALID_PTE/*, *PINVALID_PTE*/;

// We know that a PTE is in transition format if the valid bit is not set and on_disc is not set
typedef struct {
    ULONG64 always_zero:1;
    ULONG64 frame_number:40;
    ULONG64 always_zero2:1;
} TRANSITION_PTE/*, *PTRANSITION_PTE*/;

typedef struct {
    union {
        VALID_PTE memory_format;
        INVALID_PTE disc_format;
        TRANSITION_PTE transition_format;
        // This is used to represent the entire format of a PTE as a number
        // If this number is zero, then we know that the PTE has never been accessed
        ULONG64 entire_format;
    };
} PTE, *PPTE;

typedef struct {
    USHORT ages[NUMBER_OF_AGES];
} PTE_REGION_AGE_COUNT, *PPTE_REGION_AGE_COUNT;

typedef struct {
    LIST_ENTRY entry;
    ULONG64 num_regions;
    CRITICAL_SECTION lock;
} PTE_REGION_LIST, *PPTE_REGION_LIST;

typedef struct {
    LIST_ENTRY entry;
    CRITICAL_SECTION lock;
    PTE_REGION_AGE_COUNT age_count;
    ULONG active:1;
} PTE_REGION, *PPTE_REGION;

extern PPTE pte_base;
extern PPTE pte_end;

extern PPTE_REGION pte_regions;
extern PPTE_REGION pte_regions_end;

extern PPTE_REGION_LIST pte_region_age_lists;

extern PPTE pte_from_va(PVOID virtual_address);
extern PVOID va_from_pte(PPTE pte);
extern PPTE_REGION pte_region_from_pte(PPTE pte);
extern PPTE pte_from_pte_region(PPTE_REGION pte_region);

extern VOID lock_pte(PPTE pte);
extern VOID unlock_pte(PPTE pte);
extern BOOLEAN try_lock_pte(PPTE pte);

extern PTE read_pte(PPTE pte);
extern VOID write_pte(PPTE pte, PTE pte_contents);
extern BOOLEAN interlocked_write_pte(PPTE pte, PTE local);

extern VOID initialize_region_listhead(PPTE_REGION_LIST listhead);
extern VOID add_region_to_list(PPTE_REGION pte_region, PPTE_REGION_LIST listhead);
extern VOID remove_region_from_list(PPTE_REGION pte_region, PPTE_REGION_LIST listhead);
extern PPTE_REGION pop_region_from_list(PPTE_REGION_LIST listhead);

extern BOOLEAN is_region_active(PPTE_REGION pte_region);
extern VOID make_region_active(PPTE_REGION pte_region);
extern VOID make_region_inactive(PPTE_REGION pte_region);
extern VOID lock_pte_region(PPTE_REGION pte_region);
extern BOOLEAN try_lock_pte_region(PPTE_REGION pte_region);
extern VOID unlock_pte_region(PPTE_REGION pte_region);
extern VOID increase_age_count(PPTE_REGION_AGE_COUNT age_count, ULONG age);
extern PPTE_REGION get_next_active_region(PPTE_REGION pte_region);

#endif //VM_PTE_H
