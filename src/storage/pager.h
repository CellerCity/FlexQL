#ifndef PAGER_H
#define PAGER_H

#include <stdint.h>
#include <pthread.h>
#include "page.h"

#define MAX_CACHE_PAGES 10000 
#define HASH_TABLE_SIZE 20000 

// 1. The Thread-Safe Cache Node
typedef struct CacheNode {
    uint32_t page_num;
    Page* page_data;
    int is_dirty;              
    
    // --- CONCURRENCY FIELDS ---
    int pin_count;                // Prevents the LRU from evicting an active page
    pthread_rwlock_t page_lock;   // The fine-grained Read/Write lock for this specific page
    
    struct CacheNode* prev;
    struct CacheNode* next;
    struct CacheNode* hash_next;  // for handling collision(s) --> chaining
} CacheNode;

typedef struct {
    CacheNode* buckets[HASH_TABLE_SIZE];
} HashTable;

// 2. The Thread-Safe Pager
typedef struct {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    
    CacheNode* head; 
    CacheNode* tail; 
    int current_cache_size;
    HashTable* lookup_table;

    // --- GLOBAL CACHE LOCK ---
    // Protects the linked-list pointers, hash map, and cache size counters
    pthread_mutex_t global_cache_lock; 
    
} Pager;

// --- Function Prototypes ---
// pager_open() returns the one Pager shared by every connection for a given
// file path - see the registry comment in pager.c. pager_close() is a
// deliberate no-op for a connection merely detaching from a table.
// pager_retire() is the only real teardown, used solely when a table's file
// is being destroyed and rebuilt (DROP TABLE / DELETE), by whichever
// connection already holds that table's exclusive write lock.
Pager* pager_open(const char* filename);
Page* get_page(Pager* pager, uint32_t page_num);
void unpin_page(Pager* pager, uint32_t page_num, int is_dirty); // NEW: Releases the page
void pager_flush(Pager* pager, CacheNode* node);
void pager_flush_all(Pager* pager); // Flushes every dirty page WITHOUT closing the pager
void lru_evict(Pager* pager);
void pager_close(Pager* pager);
void pager_retire(const char* filename);

#endif