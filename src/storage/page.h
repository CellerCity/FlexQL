#ifndef PAGE_H
#define PAGE_H

#include <stdint.h>

// --- System & Hardware Macros ---
// 4096 bytes is the universal standard because it perfectly matches 
// the physical block size of most SSDs and Operating Systems.
#define PAGE_SIZE 4096      

// An absolute hard limit to prevent a single row from taking over a whole page
#define MAX_TUPLE_SIZE 512  

// --- Slotted Page Structures ---

/* * 1. The Slot: 
 * Every time we insert a row, we create one of these at the top of the page.
 * It tells the engine exactly where the data lives and how big it is.
 */
typedef struct {
    uint16_t offset;  // Byte offset from the start of the page where the row begins
    uint16_t length;  // How many bytes the actual row data takes up
} Slot;

/* * 2. The Page Header:
 * Sits at the absolute 0-byte mark of every 4KB page.
 */
typedef struct {
    uint8_t page_type;      // 0 for DATA, 1 for BTREE_INDEX
    uint32_t page_id;       // Unique ID for this page (e.g., Page 0, Page 1)
    uint16_t num_slots;     // How many records are currently in this page
    
    // This points to the start of the unallocated space. 
    // It starts at 4096 and shrinks downwards as we insert data at the bottom.
    uint16_t free_space_ptr; 
    
    // To satisfy the assignment's concurrency requirements later, 
    // we can add an atomic lock flag here, but we will leave it simple for now.
} PageHeader;

/*
 * 3. The Page Itself:
 * This struct perfectly maps to exactly 4096 bytes of memory.
 * We don't define the `data` array strictly, because it grows dynamically 
 * from the bottom up, while the slots grow top down.
 */
typedef struct {
    PageHeader header;
    
    // The remaining space (4096 - sizeof(PageHeader)) is raw bytes.
    // It holds the Slot[] array growing forward, and Tuple data growing backward.
    char data[PAGE_SIZE - sizeof(PageHeader)]; 
} Page;

// --- Expiration Requirement ---
// The assignment requires an expiration timestamp for each inserted row.
// We can prepend this small header to every row's raw data before saving it.
typedef struct {
    uint64_t expiration_timestamp; 
} TupleHeader;

#endif