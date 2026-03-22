#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btree.h"
#include "pager.h"

// Helper function to compare two generic IndexKeys
int compare_keys(IndexKey a, IndexKey b) {
    // If types don't match, we have a schema error, but we'll default to 0 to avoid crashing
    if (a.type != b.type) return 0; 

    switch (a.type) {
        case 1: // INT
            if (a.value.int_val < b.value.int_val) return -1;
            if (a.value.int_val > b.value.int_val) return 1;
            return 0;
            
        case 2: // DECIMAL
            if (a.value.dec_val < b.value.dec_val) return -1;
            if (a.value.dec_val > b.value.dec_val) return 1;
            return 0;
            
        case 3: // DATETIME (int64_t)
            if (a.value.dt_val < b.value.dt_val) return -1;
            if (a.value.dt_val > b.value.dt_val) return 1;
            return 0;
            
        case 4: // VARCHAR (Fixed 32-byte limit for index keys)
            // strncmp perfectly handles this by returning <0, 0, or >0
            return strncmp(a.value.str_val, b.value.str_val, 32); 
            
        default:
            return 0;
    }
}



// Searches the B+ Tree for a specific key.
// Returns 1 if found (and populates 'result'), returns 0 if not found.
int btree_search(Pager* pager, uint32_t root_page_id, IndexKey search_key, RecordID* result) {
    
    uint32_t current_page_id = root_page_id;

    // We loop until we hit a Leaf Node, then we break.
    while (1) {
        // 1. Fetch the page from our Pager (Cache / Disk)
        Page* page = get_page(pager, current_page_id);
        
        // 2. Cast the raw data area into our BTreeNode struct
        // Remember: The BTreeNode lives inside the page->data array!
        BTreeNode* node = (BTreeNode*)(page->data);

        // 3. Find the first key in the node that is >= our search_key
        int i = 0;
        while (i < node->num_keys && compare_keys(search_key, node->keys[i]) > 0) {
            i++;
        }

        // 4. Branch based on Node Type
        if (node->is_leaf) {
            // We are at the bottom of the tree! 
            // Check if the key we stopped at is an exact match.
            if (i < node->num_keys && compare_keys(search_key, node->keys[i]) == 0) {
                // MATCH FOUND! 
                result->page_num = node->payload.leaf_data.records[i].page_num;
                result->slot_num = node->payload.leaf_data.records[i].slot_num;
                return 1; 
            } else {
                // The key does not exist in the database.
                return 0; 
            }
        } else {
            // We are in an Internal Node. 
            // The index 'i' corresponds exactly to the child pointer we need to follow.
            current_page_id = node->payload.child_pages[i];
        }
    }
}




// src/storage/btree.c

// Helper: Inserts a key and record into a leaf node that is GUARANTEED to have space.
void insert_into_leaf(BTreeNode* leaf, IndexKey key, RecordID record) {
    int i = 0;
    
    // 1. Find the exact index where this key belongs
    while (i < leaf->num_keys && compare_keys(key, leaf->keys[i]) > 0) {
        i++;
    }

    // 2. Shift all keys and records to the right to make room
    for (int j = leaf->num_keys; j > i; j--) {
        leaf->keys[j] = leaf->keys[j - 1];
        leaf->payload.leaf_data.records[j] = leaf->payload.leaf_data.records[j - 1];
    }

    // 3. Insert the new key and record
    leaf->keys[i] = key;
    leaf->payload.leaf_data.records[i] = record;
    leaf->num_keys++;
}



// src/storage/btree.c

void btree_insert(Pager* pager, uint32_t* root_page_id, IndexKey key, RecordID record) {
    uint32_t current_page_id = *root_page_id;
    Page* page = get_page(pager, current_page_id);
    BTreeNode* node = (BTreeNode*)(page->data);

    // 1. If the tree is completely empty, initialize the root as a leaf
    if (node->num_keys == 0 && node->is_root) {
        node->is_leaf = 1;
        insert_into_leaf(node, key, record);
        pager_flush(pager, current_page_id);
        return;
    }

    // 2. Traverse down to the correct Leaf Node
    while (!node->is_leaf) {
        int i = 0;
        while (i < node->num_keys && compare_keys(key, node->keys[i]) >= 0) {
            i++;
        }
        current_page_id = node->payload.child_pages[i];
        page = get_page(pager, current_page_id);
        node = (BTreeNode*)(page->data);
    }

    // 3. We are now at the Leaf Node. Check if it's full.
    if (node->num_keys < MAX_KEYS) {
        // --- THE HAPPY PATH ---
        insert_into_leaf(node, key, record);
        pager_flush(pager, current_page_id);
    } else {
        // --- THE SPLIT PATH ---
        // The node has 110 keys. We must split it!
        split_leaf_node(pager, root_page_id, page, key, record);
    }
}




// src/storage/btree.c

void split_leaf_node(Pager* pager, uint32_t* root_page_id, Page* old_page, IndexKey new_key, RecordID new_record) {
    BTreeNode* old_node = (BTreeNode*)old_page->data;
    
    // 1. Allocate a brand new page for the right half of the split
    uint32_t new_page_id = pager->num_pages; 
    Page* new_page = get_page(pager, new_page_id);
    BTreeNode* new_node = (BTreeNode*)new_page->data;
    
    // Initialize the new node
    new_node->is_leaf = 1;
    new_node->is_root = 0;
    new_node->num_keys = 0;
    new_node->parent_page_id = old_node->parent_page_id;

    // 2. Create a temporary array to hold ALL keys (MAX_KEYS + 1) to easily sort them
    IndexKey temp_keys[MAX_KEYS + 1];
    RecordID temp_records[MAX_KEYS + 1];
    
    int insert_idx = 0;
    while (insert_idx < MAX_KEYS && compare_keys(new_key, old_node->keys[insert_idx]) > 0) {
        insert_idx++;
    }
    
    // Copy everything into the temp arrays, inserting the new key at the right spot
    for (int i = 0, j = 0; i < old_node->num_keys + 1; i++, j++) {
        if (j == insert_idx) {
            temp_keys[j] = new_key;
            temp_records[j] = new_record;
            i--; // Hold the old_node index back one step
        } else {
            temp_keys[j] = old_node->keys[i];
            temp_records[j] = old_node->payload.leaf_data.records[i];
        }
    }

    // 3. Distribute the keys: Left gets half, Right gets half
    int split_point = (MAX_KEYS + 1) / 2;
    
    old_node->num_keys = split_point;
    for (int i = 0; i < split_point; i++) {
        old_node->keys[i] = temp_keys[i];
        old_node->payload.leaf_data.records[i] = temp_records[i];
    }
    
    new_node->num_keys = (MAX_KEYS + 1) - split_point;
    for (int i = split_point, j = 0; i < MAX_KEYS + 1; i++, j++) {
        new_node->keys[j] = temp_keys[i];
        new_node->payload.leaf_data.records[j] = temp_records[i];
    }

    // 4. Update the linked list pointers (for fast range scans)
    new_node->payload.leaf_data.next_leaf_page = old_node->payload.leaf_data.next_leaf_page;
    old_node->payload.leaf_data.next_leaf_page = new_page_id;

    // 5. The Magic Step: Push the middle key UP to the parent!
    IndexKey middle_key = new_node->keys[0]; // The smallest key in the new right node
    
    if (old_node->is_root) {
        // If the root split, we must create a NEW root!
        create_new_root(pager, root_page_id, old_page->header.page_id, new_page_id, middle_key);
    } else {
        // Otherwise, insert the middle key into the existing parent internal node
        insert_into_internal(pager, root_page_id, old_node->parent_page_id, middle_key, new_page_id);
    }

    // 6. Flush our changes to disk
    pager_flush(pager, old_page->header.page_id);
    pager_flush(pager, new_page_id);
}





