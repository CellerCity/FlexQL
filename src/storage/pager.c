#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include "pager.h"

// --- Helper: Hash Function ---
uint32_t hash_page(uint32_t page_num) {
    return page_num % HASH_TABLE_SIZE;
}

static void pager_close_raw(Pager* pager);

// --- 0. The Shared Pager Registry ---
// Every field below - the global_cache_lock mutex, the per-page rwlock, the
// pin_count - was already built to make a Pager safe to share across
// threads. But every caller opened its OWN private Pager per connection for
// the same file, so none of that machinery ever saw real cross-thread
// contention. Two connections writing the same table each had their own
// disconnected view of it: one connection's newly-written pages were
// invisible to another's cache and, worse, to another's num_pages
// bookkeeping - which could make an already-written page look "brand new"
// and hand back malloc()'s uninitialized garbage instead of the real page.
// Confirmed live: this crashed the server outright (SIGSEGV in the B+Tree
// search reading garbage "keys").
//
// The fix is to actually share one Pager per file across every connection,
// which is what this registry does. pager_open() below now returns the same
// Pager instance to every caller for the same path instead of opening a new
// one; pager_close() from a connection just detaching from a table is now a
// no-op (the pager isn't this connection's to destroy), and pager_retire()
// is the only thing that actually closes and forgets a shared pager - used
// solely by DROP TABLE and the DELETE truncate/rebuild path, both of which
// already hold that table's exclusive write lock when they call it.
#define MAX_OPEN_PAGERS 200
typedef struct {
    char filepath[512];
    Pager* pager;
} PagerRegistryEntry;

static PagerRegistryEntry pager_registry[MAX_OPEN_PAGERS];
static int num_open_pagers = 0;
static pthread_mutex_t pager_registry_lock = PTHREAD_MUTEX_INITIALIZER;

// --- 1. Open the Database and Initialize LRU ---
// The actual file-open logic, run once per distinct file path for the life
// of the server. Never call this directly - go through pager_open() below.
static Pager* pager_open_raw(const char* filename) {
    int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        // This used to exit(EXIT_FAILURE) here, which took the whole server
        // down - every connected client - over one bad request (e.g. a table
        // name long enough that DATA_DIR/db/name.dat exceeds the filesystem's
        // NAME_MAX). Fail this one call instead; the caller reports an error
        // to just its own client.
        perror("[-] Unable to open database file");
        return NULL;
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

// The public entry point - every caller in the codebase already calls this
// exact name, so callers needed no changes. Returns the one shared Pager
// for this path, opening it the first time and handing back the same
// instance to every later caller.
Pager* pager_open(const char* filename) {
    pthread_mutex_lock(&pager_registry_lock);

    for (int i = 0; i < num_open_pagers; i++) {
        if (strcmp(pager_registry[i].filepath, filename) == 0) {
            Pager* existing = pager_registry[i].pager;
            pthread_mutex_unlock(&pager_registry_lock);
            return existing;
        }
    }

    if (num_open_pagers >= MAX_OPEN_PAGERS) {
        pthread_mutex_unlock(&pager_registry_lock);
        fprintf(stderr, "[-] Too many distinct open tables this server run (limit %d).\n", MAX_OPEN_PAGERS);
        return NULL;
    }

    Pager* pager = pager_open_raw(filename);
    if (pager == NULL) {
        pthread_mutex_unlock(&pager_registry_lock);
        return NULL;
    }

    strncpy(pager_registry[num_open_pagers].filepath, filename, sizeof(pager_registry[0].filepath) - 1);
    pager_registry[num_open_pagers].filepath[sizeof(pager_registry[0].filepath) - 1] = '\0';
    pager_registry[num_open_pagers].pager = pager;
    num_open_pagers++;

    pthread_mutex_unlock(&pager_registry_lock);
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
    // calloc, not malloc: a brand-new page only gets its header fields set
    // below (page_id, num_slots, free_space_ptr) - num_slots/num_keys = 0
    // means nothing downstream ever reads the rest of the buffer, but the
    // unused bytes still got written to disk as raw heap garbage on every
    // flush. valgrind flagged this directly (uninitialised bytes passed to
    // write()); zeroing it here is one line and removes the whole class.
    new_node->page_data = calloc(1, sizeof(Page));
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


// --- 6. Flush every dirty page, but keep the pager (and its cache) alive ---
// Sharing one Pager per file (see the registry above) fixed cross-connection
// *visibility* - a write is immediately visible to every other connection
// via the shared in-memory cache. It does NOT by itself make anything
// durable: nothing writes a page to disk anymore except LRU eviction, since
// a connection detaching no longer closes (and flushes) the pager.
//
// That's a real gap, not just a performance one: execute_create()'s "this
// table's file already exists, so it survived the crash - skip
// re-initializing it" shortcut assumes a table's root page is on disk the
// moment the file exists. Confirmed live: without this, CREATE TABLE
// followed immediately by kill -9 leaves a genuine 0-byte .dat file: the
// next boot's replay sees the file "exists", trusts it, and hands
// btree_search a root page that's never been initialized - is_leaf and the
// child-page array are malloc()'s uninitialized garbage, and the search
// spins forever routing through garbage child pointers. Confirmed via a
// live repro that this hangs the server (not a mutex deadlock - traced with
// instrumented get_page() calls showing an actual infinite loop).
//
// Called once after a table's root page is created/rebuilt (so file
// existence is trustworthy again) and once at the end of every INSERT
// batch, alongside the WAL fflush that already happens at the same
// granularity - so a completed statement's data is durable before the lock
// protecting it is released, not left to accumulate in RAM indefinitely.
void pager_flush_all(Pager* pager) {
    pthread_mutex_lock(&pager->global_cache_lock);
    CacheNode* curr = pager->head;
    while (curr != NULL) {
        if (curr->is_dirty) {
            pager_flush(pager, curr);
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&pager->global_cache_lock);
}

// --- 7. Safely Close and Flush Everything ---
// The actual teardown logic. Never call this directly except from
// pager_retire() below - it destroys state every other connection sharing
// this Pager is relying on.
static void pager_close_raw(Pager* pager) {
    // Destroy the global lock
    pthread_mutex_destroy(&pager->global_cache_lock);

    CacheNode* curr = pager->head;
    while (curr != NULL) {
        pthread_rwlock_destroy(&curr->page_lock);

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

// A connection detaching from a table (switching to another table, or
// disconnecting) does NOT own the shared Pager and must not destroy it -
// other connections may still be using it. This is now a deliberate no-op;
// every caller in the codebase already calls pager_close() at exactly the
// points where it used to detach, so none of those call sites needed to
// change.
void pager_close(Pager* pager) {
    (void)pager;
}

// The only real teardown path: used solely when a table's underlying file
// is being destroyed and rebuilt (DROP TABLE, or DELETE's truncate/rebuild),
// so any registry entry pointing at the old file must be retired before a
// fresh one can be opened at the same path. Both callers already hold that
// table's exclusive write lock, so no other connection can be inside
// execute_insert/execute_select for this table concurrently.
void pager_retire(const char* filename) {
    pthread_mutex_lock(&pager_registry_lock);

    for (int i = 0; i < num_open_pagers; i++) {
        if (strcmp(pager_registry[i].filepath, filename) == 0) {
            pager_close_raw(pager_registry[i].pager);
            pager_registry[i] = pager_registry[num_open_pagers - 1];
            num_open_pagers--;
            break;
        }
    }

    pthread_mutex_unlock(&pager_registry_lock);
}