#ifndef PAGER_H
#define PAGER_H

#include <stdint.h>
#include "page.h"

// Define a reasonable limit for how many pages we can hold in RAM at once.
// 10,000 pages * 4KB = ~40 Megabytes of RAM. Very lightweight!
#define MAX_PAGES 10000 

typedef struct {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    
    // Array of pointers to pages currently loaded in memory (The Cache)
    Page* pages[MAX_PAGES]; 
} Pager;

// --- Function Prototypes ---
Pager* pager_open(const char* filename);
Page* get_page(Pager* pager, uint32_t page_num);
void pager_flush(Pager* pager, uint32_t page_num);
void pager_close(Pager* pager);

#endif