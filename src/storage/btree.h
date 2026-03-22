#ifndef BTREE_H
#define BTREE_H

#include <stdint.h>
#include "page.h"
#include "pager.h"

// --- B+ Tree Configuration ---
// The PageHeader is ~12 bytes. 4096 - 12 = 4084 bytes available for our BTreeNode.
// We configure our MAX_KEYS so the struct easily fits within this limit.

// --- Record Pointer ---
// This is what the Leaf Node returns when it finds a match!
// It tells the execution engine EXACTLY where to find the row.
typedef struct {
    uint32_t page_num;
    uint16_t slot_num;
} RecordID;


// --- The Generic Index Key ---
// This struct is always exactly 36 bytes, no matter what type it holds.
typedef struct {
    int type; // 1=INT, 2=DECIMAL, 3=DATETIME, 4=VARCHAR
    union {
        int32_t int_val;
        double dec_val;
        int64_t dt_val;
        char str_val[32]; // Fixed cap for string keys
    } value;
} IndexKey;


// --- The Updated B+ Tree Node ---
// Because IndexKey is 36 bytes, we must reduce MAX_KEYS to fit inside the 4096-byte page.
// (4096 bytes / 36 bytes) = Roughly 110 keys max per node.
#define MAX_KEYS 110 

typedef struct {
    uint8_t is_leaf;          
    uint8_t is_root;          
    uint16_t num_keys;        
    uint32_t parent_page_id;  

    // Now our tree can hold ANY data type!
    IndexKey keys[MAX_KEYS];

    union {
        uint32_t child_pages[MAX_KEYS + 1];
        struct {
            RecordID records[MAX_KEYS];
            uint32_t next_leaf_page; 
        } leaf_data;
    } payload;

} BTreeNode;


// --- Function Prototypes ---
// Core logic for searching and inserting
RecordID* btree_search(Pager* pager, uint32_t root_page_id, uint32_t search_key);
void btree_insert(Pager* pager, uint32_t root_page_id, uint32_t key, RecordID record);

#endif