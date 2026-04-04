#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include "pager.h"

// --- Helper: Hash Function ---
uint32_t hash_page(uint32_t page_num) {
    return page_num % HASH_TABLE_SIZE;
}

// --- 1. Open the Database and Initialize LRU ---
Pager* pager_open(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        perror("[-] Unable to open database file");
        exit(EXIT_FAILURE);
    }

    
    off_t file_length = lseek(fd, 0, SEEK_END);
    
    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = file_length;
    pager->num_pages = (file_length / PAGE_SIZE);

    // Initialize the Global Cache Lock
    pthread_mutex_init(&pager->global_cache_lock, NULL);

    // Initialize LRU List
    pager->head = NULL;
    pager->tail = NULL;
    pager->current_cache_size = 0;

    // Initialize Hash Table
    pager->lookup_table = malloc(sizeof(HashTable));
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        pager->lookup_table->buckets[i] = NULL;
    }

    return pager;
}

// --- 2. The Flush Function ---
void pager_flush(Pager* pager, CacheNode* node) {
    if (node == NULL || !node->is_dirty) return;

    uint32_t offset = node->page_num * PAGE_SIZE;
    lseek(pager->file_descriptor, offset, SEEK_SET);
    
    ssize_t bytes_written = write(pager->file_descriptor, node->page_data, PAGE_SIZE);
    if (bytes_written == -1) {
        perror("[-] Error writing to disk");
        exit(EXIT_FAILURE);
    }
    
    node->is_dirty = 0; // Reset dirty bit after saving
}

// --- 3. The LRU Eviction (The Bouncer) ---
void lru_evict(Pager* pager) {

    // Note: This function assumes the caller already holds the global_cache_lock!
    
    if (pager->head == NULL) return;

    // Start at the head (Oldest), but keep looking if it is pinned!
    CacheNode* evict_node = pager->head;
    while (evict_node != NULL && evict_node->pin_count > 0) {
        evict_node = evict_node->next;
    }

    // If every single page in the cache is currently being used, we are out of memory.
    if (evict_node == NULL) {
        printf("[-] FATAL: Cache is full and all pages are pinned!\n");
        exit(EXIT_FAILURE); 
    }

    // ... (The rest of the eviction logic remains exactly the same: flush if dirty, remove from DLL/Hash Map, and free memory) ...


    // Flush to disk ONLY if modified
    if (evict_node->is_dirty) {
        pager_flush(pager, evict_node);
    }

    // Remove from Hash Map
    uint32_t bucket_idx = hash_page(evict_node->page_num);
    CacheNode* curr = pager->lookup_table->buckets[bucket_idx];
    CacheNode* prev = NULL;
    
    while (curr != NULL) {
        if (curr->page_num == evict_node->page_num) {
            if (prev == NULL) pager->lookup_table->buckets[bucket_idx] = curr->hash_next;
            else prev->hash_next = curr->hash_next;
            break;
        }
        prev = curr;
        curr = curr->hash_next;
    }

    // Remove from DLL
    pager->head = evict_node->next;
    if (pager->head != NULL) pager->head->prev = NULL;
    else pager->tail = NULL;

    pager->current_cache_size--;
    
    pthread_rwlock_destroy(&evict_node->page_lock); // Destroy the lock before freeing
    
    free(evict_node->page_data);
    free(evict_node);
}

// --- 4. The Core Logic: Get Page ---
Page* get_page(Pager* pager, uint32_t page_num) {
    // 1. Lock the entire cache structure before touching pointers
    pthread_mutex_lock(&pager->global_cache_lock);

    uint32_t bucket_idx = hash_page(page_num);
    CacheNode* node = pager->lookup_table->buckets[bucket_idx];

    // --- CACHE HIT ---
    while (node != NULL) {
        if (node->page_num == page_num) {
            
            // --- ACTUAL MRU LOGIC (Move to Tail) ---
            if (node != pager->tail) {
                // 1. Disconnect from current position
                if (node->prev) node->prev->next = node->next;
                else pager->head = node->next; // It was head

                if (node->next) node->next->prev = node->prev;

                // 2. Attach to tail
                node->prev = pager->tail;
                node->next = NULL;
                if (pager->tail) pager->tail->next = node;
                pager->tail = node;
            }
            // ----------------------------------------
            
            node->pin_count++; // Pin it so the bouncer doesn't grab it
            pthread_mutex_unlock(&pager->global_cache_lock); // Release global lock
            return node->page_data;
        }
        node = node->hash_next;
    }

    // --- CACHE MISS ---
    if (pager->current_cache_size >= MAX_CACHE_PAGES) {
        lru_evict(pager); // Bouncer runs while global lock is held
    }

    CacheNode* new_node = malloc(sizeof(CacheNode));
    new_node->page_num = page_num;
    new_node->page_data = malloc(sizeof(Page));
    new_node->is_dirty = 0;
    
    // Initialize concurrency state
    new_node->pin_count = 1; 
    pthread_rwlock_init(&new_node->page_lock, NULL);

    // Read from disk or initialize empty page
    if (page_num < pager->num_pages) {
        uint32_t offset = page_num * PAGE_SIZE;
        lseek(pager->file_descriptor, offset, SEEK_SET);
        read(pager->file_descriptor, new_node->page_data, PAGE_SIZE);
    } else {
        new_node->page_data->header.page_id = page_num;
        new_node->page_data->header.num_slots = 0;
        new_node->page_data->header.free_space_ptr = PAGE_SIZE;
        // If we are creating a brand new page, it must be saved to disk eventually!
        new_node->is_dirty = 1; 

        // --- Update total pages! ---
        if (page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }

    // Add to Hash Map (Insert at head of bucket for speed)
    new_node->hash_next = pager->lookup_table->buckets[bucket_idx];
    pager->lookup_table->buckets[bucket_idx] = new_node;

    // Add to DLL Tail (Most Recently Used)
    new_node->next = NULL;
    if (pager->tail == NULL) {
        new_node->prev = NULL;
        pager->head = new_node;
        pager->tail = new_node;
    } else {
        new_node->prev = pager->tail;
        pager->tail->next = new_node;
        pager->tail = new_node;
    }

    pager->current_cache_size++;
    pthread_mutex_unlock(&pager->global_cache_lock); // Release global lock
    return new_node->page_data;
}

// --- 5. The Engine must release the page when done! ---
void unpin_page(Pager* pager, uint32_t page_num, int is_dirty) {
    pthread_mutex_lock(&pager->global_cache_lock);
    
    uint32_t bucket_idx = hash_page(page_num);
    CacheNode* node = pager->lookup_table->buckets[bucket_idx];
    
    while (node != NULL) {
        if (node->page_num == page_num) {
            node->pin_count--;
            if (is_dirty) {
                node->is_dirty = 1; // Mark for eventual flush
            }
            break;
        }
        node = node->hash_next;
    }
    
    pthread_mutex_unlock(&pager->global_cache_lock);
}


// --- 6. Safely Close and Flush Everything ---
void pager_close(Pager* pager) {
    // Destroy the global lock
    pthread_mutex_destroy(&pager->global_cache_lock);

    CacheNode* curr = pager->head;
    while (curr != NULL) {
        pthread_rwlock_destroy(&curr->page_lock);
        
        // ... (free logic) ...
        CacheNode* next = curr->next;
        if (curr->is_dirty) {
            pager_flush(pager, curr);
        }
        free(curr->page_data);
        free(curr);
        curr = next;
    }

    free(pager->lookup_table);
    close(pager->file_descriptor);
    free(pager);
}