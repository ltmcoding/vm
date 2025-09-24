# VM
## A fully working virtual memory management state machine
### This program uses multi-threading to manage memory more effectively. It creates and manages virtual address space, holding a physical page pool and allocating a paging file much larger than its page pool in order to illustrate the effectiveness of virtual memory. It controls the entire process of memory management: managing free, standby, and modified PFN lists; handling page faults; aging and trimming active pages; writing modified pages to the paging file; repurposing standby pages; and retrieving and remapping pages from the paging file into physical memory.

For a surface-level explanation of the memory manager, click here: https://docs.google.com/document/d/1152bx53z44v_KMxqAU13WA0q--mN1a79ag8CcTIqHV8/edit?usp=sharing

For a deeper look at how my program's strategy and design work, click here: https://www.menloschool.org/live/files/4094-landon-moceri-round-table-spring-edition-pdf

If you have any inquiries about this project, feel free to email me at: landon@ltmcoding.dev
