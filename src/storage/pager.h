#ifndef PAGER_H
#define PAGER_H

#include <stdint.h>
#include "page.h"

#define MAX_CACHE_PAGES 10000 
#define HASH_TABLE_SIZE 20000 

// 1. The Doubly-Linked List Node
typedef struct CacheNode {
    uint32_t page_num;
    Page* page_data;
    int is_dirty;              // 1 if modified, 0 if clean 
    
    // Pointers for the LRU Doubly-Linked List
    struct CacheNode* prev;
    struct CacheNode* next;
    
    // Pointer for the Hash Map collision chain (THE MISSING PIECE!)
    struct CacheNode* hash_next; 
} CacheNode;

// 2. The Lookup Table (Hash Map)
typedef struct {
    CacheNode* buckets[HASH_TABLE_SIZE];
} HashTable;

// 3. The Upgraded Pager
typedef struct {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    
    // The LRU List Pointers
    CacheNode* head; // Least Recently Used (Evict from here)
    CacheNode* tail; // Most Recently Used (Insert here)
    int current_cache_size;
    
    // The O(1) Lookup Table
    HashTable* lookup_table;
    
} Pager;

// --- Function Prototypes ---
Pager* pager_open(const char* filename);
Page* get_page(Pager* pager, uint32_t page_num);
void pager_flush(Pager* pager, CacheNode* node);
void lru_evict(Pager* pager); 
void pager_close(Pager* pager);

#endif