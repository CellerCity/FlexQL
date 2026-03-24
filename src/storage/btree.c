#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btree.h"
#include "pager.h"

// --- Prototypes ---
void split_leaf_node(Pager* pager, uint32_t* root_page_id, Page* old_page, IndexKey new_key, RecordID new_record);
void create_new_root(Pager* pager, uint32_t* root_page_id, uint32_t left_page_id, uint32_t right_page_id, IndexKey key);
void insert_into_internal(Pager* pager, uint32_t* root_page_id, uint32_t parent_page_id, IndexKey key, uint32_t right_child_page_id);
uint32_t find_parent(Pager* pager, uint32_t root_page_id, uint32_t target_page_id);


// --- THE BINARY SEARCH OPTIMIZATION ---
// Drops array traversals from O(N) to O(log N)
int get_btree_index(BTreeNode* node, IndexKey search_key, int is_internal) {
    int left = 0;
    int right = node->num_keys - 1;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        int cmp = compare_keys(search_key, node->keys[mid]);
        
        if (cmp > 0) {
            left = mid + 1;
        } else if (cmp < 0) {
            right = mid - 1;
        } else {
            // EXACT MATCH FOUND!
            // If it's a leaf, we want this exact spot. 
            // If internal, we want the child pointer just to the right of it.
            return is_internal ? (mid + 1) : mid;
        }
    }
    return left; 
}


int compare_keys(IndexKey a, IndexKey b) {
    if (a.type != b.type) return 0; 
    switch (a.type) {
        case 1: 
            if (a.value.int_val < b.value.int_val) return -1;
            if (a.value.int_val > b.value.int_val) return 1;
            return 0;
        case 2: 
            if (a.value.dec_val < b.value.dec_val) return -1;
            if (a.value.dec_val > b.value.dec_val) return 1;
            return 0;
        case 3: 
            if (a.value.dt_val < b.value.dt_val) return -1;
            if (a.value.dt_val > b.value.dt_val) return 1;
            return 0;
        case 4: 
            return strncmp(a.value.str_val, b.value.str_val, 32); 
        default: return 0;
    }
}

int btree_search(Pager* pager, uint32_t root_page_id, IndexKey search_key, RecordID* result) {
    uint32_t current_page_id = root_page_id;

    while (1) {
        Page* page = get_page(pager, current_page_id);
        BTreeNode* node = (BTreeNode*)(page->data);

        if (node->is_leaf) {
            // THE FIX: Instant O(log N) lookup for Leaf Nodes
            int i = get_btree_index(node, search_key, 0);
            
            if (i < node->num_keys && compare_keys(search_key, node->keys[i]) == 0) {
                result->page_num = node->payload.leaf_data.records[i].page_num;
                result->slot_num = node->payload.leaf_data.records[i].slot_num;
                unpin_page(pager, current_page_id, 0); // Release read lock
                return 1; 
            } else {
                unpin_page(pager, current_page_id, 0); 
                return 0; 
            }
        } else {
            // THE FIX: Instant O(log N) lookup for Internal Nodes
            int i = get_btree_index(node, search_key, 1);

            
            uint32_t next_page = node->payload.child_pages[i];
            unpin_page(pager, current_page_id, 0); // Release parent before traversing child
            current_page_id = next_page;
        }
    }
}

void insert_into_leaf(BTreeNode* leaf, IndexKey key, RecordID record) {
    int i = 0;
    while (i < leaf->num_keys && compare_keys(key, leaf->keys[i]) > 0) i++;
    for (int j = leaf->num_keys; j > i; j--) {
        leaf->keys[j] = leaf->keys[j - 1];
        leaf->payload.leaf_data.records[j] = leaf->payload.leaf_data.records[j - 1];
    }
    leaf->keys[i] = key;
    leaf->payload.leaf_data.records[i] = record;
    leaf->num_keys++;
}

void btree_insert(Pager* pager, uint32_t* root_page_id, IndexKey key, RecordID record) {
    uint32_t current_page_id = *root_page_id;
    Page* page = get_page(pager, current_page_id);
    BTreeNode* node = (BTreeNode*)(page->data);

    if (node->num_keys == 0 && node->is_root) {
        node->is_leaf = 1;
        insert_into_leaf(node, key, record);
        unpin_page(pager, current_page_id, 1); // Mark dirty and release
        return;
    }

    while (!node->is_leaf) {
        // THE FIX: Instant O(log N) routing during inserts!
        int i = get_btree_index(node, key, 1);

        uint32_t next_page_id = node->payload.child_pages[i];
        
        unpin_page(pager, current_page_id, 0); // Release parent
        current_page_id = next_page_id;
        page = get_page(pager, current_page_id);
        node = (BTreeNode*)(page->data);
    }

    if (node->num_keys < MAX_KEYS) {
        insert_into_leaf(node, key, record);
        unpin_page(pager, current_page_id, 1);
    } else {
        split_leaf_node(pager, root_page_id, page, key, record);
    }
}

void split_leaf_node(Pager* pager, uint32_t* root_page_id, Page* old_page, IndexKey new_key, RecordID new_record) {
    BTreeNode* old_node = (BTreeNode*)old_page->data;
    uint32_t old_page_id = old_page->header.page_id;
    
    uint32_t new_page_id = pager->num_pages; 
    Page* new_page = get_page(pager, new_page_id);
    BTreeNode* new_node = (BTreeNode*)new_page->data;
    
    new_node->is_leaf = 1;
    new_node->is_root = 0;
    new_node->num_keys = 0;
    new_node->parent_page_id = old_node->parent_page_id;

    IndexKey temp_keys[MAX_KEYS + 1];
    RecordID temp_records[MAX_KEYS + 1];
    
    int insert_idx = 0;
    while (insert_idx < MAX_KEYS && compare_keys(new_key, old_node->keys[insert_idx]) > 0) insert_idx++;
    
    // Copy everything into the temp arrays safely!
    for (int i = 0, j = 0; j < old_node->num_keys + 1; j++) {
        if (j == insert_idx) {
            temp_keys[j] = new_key;
            temp_records[j] = new_record;
        } else {
            temp_keys[j] = old_node->keys[i];
            temp_records[j] = old_node->payload.leaf_data.records[i];
            i++; 
        }
    }
    
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

    new_node->payload.leaf_data.next_leaf_page = old_node->payload.leaf_data.next_leaf_page;
    old_node->payload.leaf_data.next_leaf_page = new_page_id;

    IndexKey middle_key = new_node->keys[0]; 
    
    if (old_node->is_root) {
        create_new_root(pager, root_page_id, old_page_id, new_page_id, middle_key);
    } else {
        // Dynamically find the TRUE parent, ignoring the ghost pointer!
        uint32_t true_parent_id = find_parent(pager, *root_page_id, old_page_id);
        insert_into_internal(pager, root_page_id, true_parent_id, middle_key, new_page_id);
    }

    unpin_page(pager, old_page_id, 1);
    unpin_page(pager, new_page_id, 1);
}

void create_new_root(Pager* pager, uint32_t* root_page_id, uint32_t left_page_id, uint32_t right_page_id, IndexKey key) {
    uint32_t new_root_id = pager->num_pages;
    Page* root_page = get_page(pager, new_root_id);
    BTreeNode* root_node = (BTreeNode*)root_page->data;

    root_node->is_leaf = 0;
    root_node->is_root = 1;
    root_node->num_keys = 1;
    root_node->keys[0] = key;
    root_node->payload.child_pages[0] = left_page_id;
    root_node->payload.child_pages[1] = right_page_id;

    Page* left_page = get_page(pager, left_page_id);
    BTreeNode* left_node = (BTreeNode*)left_page->data;
    left_node->parent_page_id = new_root_id;
    left_node->is_root = 0; 
    unpin_page(pager, left_page_id, 1);

    Page* right_page = get_page(pager, right_page_id);
    BTreeNode* right_node = (BTreeNode*)right_page->data;
    right_node->parent_page_id = new_root_id;
    right_node->is_root = 0;
    unpin_page(pager, right_page_id, 1);

    unpin_page(pager, new_root_id, 1);
    *root_page_id = new_root_id;
}



uint32_t find_parent(Pager* pager, uint32_t root_page_id, uint32_t target_page_id) {
    if (root_page_id == target_page_id) return 0; // The root has no parent!

    // 1. Get the "GPS Coordinate" (the first key) of the target page
    Page* target_page = get_page(pager, target_page_id);
    BTreeNode* target_node = (BTreeNode*)target_page->data;
    IndexKey routing_key = target_node->keys[0]; 
    unpin_page(pager, target_page_id, 0);

    uint32_t current_page_id = root_page_id;

    // 2. Traverse down the tree using the routing key
    while (1) {
        Page* page = get_page(pager, current_page_id);
        BTreeNode* node = (BTreeNode*)page->data;

        // If we somehow hit a leaf, the parent doesn't exist (safety catch)
        if (node->is_leaf) {
            unpin_page(pager, current_page_id, 0);
            return 0; 
        }

        // 3. Check if one of THIS node's children is the target page
        for (int i = 0; i <= node->num_keys; i++) {
            if (node->payload.child_pages[i] == target_page_id) {
                unpin_page(pager, current_page_id, 0);
                return current_page_id; // We found the parent!
            }
        }

        // 4. If not found, figure out which child branch to follow
        int i = 0;
        while (i < node->num_keys && compare_keys(routing_key, node->keys[i]) >= 0) {
            i++;
        }
        
        uint32_t next_page_id = node->payload.child_pages[i];
        unpin_page(pager, current_page_id, 0); // Unpin before moving down
        current_page_id = next_page_id;
    }
}


void insert_into_internal(Pager* pager, uint32_t* root_page_id, uint32_t parent_page_id, IndexKey key, uint32_t right_child_page_id) {
    Page* parent_page = get_page(pager, parent_page_id);
    BTreeNode* parent_node = (BTreeNode*)parent_page->data;

    // ==========================================
    // THE INTERNAL NODE SPLIT LOGIC
    // ==========================================
    if (parent_node->num_keys >= MAX_KEYS) {
        
        // 1. Create temporary arrays to hold the overflow (MAX_KEYS + 1)
        IndexKey temp_keys[MAX_KEYS + 1];
        uint32_t temp_children[MAX_KEYS + 2];

        // Copy existing keys and children to temp arrays
        for (int i = 0; i < MAX_KEYS; i++) {
            temp_keys[i] = parent_node->keys[i];
            temp_children[i] = parent_node->payload.child_pages[i];
        }
        temp_children[MAX_KEYS] = parent_node->payload.child_pages[MAX_KEYS];

        // Find where the new key should be inserted in the temp arrays
        int insert_idx = 0;
        while (insert_idx < MAX_KEYS && compare_keys(key, temp_keys[insert_idx]) > 0) {
            insert_idx++;
        }

        // Shift to make room for the new key and child pointer
        for (int j = MAX_KEYS; j > insert_idx; j--) {
            temp_keys[j] = temp_keys[j - 1];
            temp_children[j + 1] = temp_children[j];
        }
        
        // Insert the new key and child
        temp_keys[insert_idx] = key;
        temp_children[insert_idx + 1] = right_child_page_id;

        // 2. Determine the middle index to split and promote
        int split_idx = (MAX_KEYS + 1) / 2;
        IndexKey promoted_key = temp_keys[split_idx];

        // 3. Create the new Right Internal Node
        uint32_t new_right_page_id = pager->num_pages;
        Page* new_right_page = get_page(pager, new_right_page_id);
        new_right_page->header.page_type = 1; // Mark as an Index Page
        
        BTreeNode* right_node = (BTreeNode*)new_right_page->data;
        right_node->is_leaf = 0;
        right_node->is_root = 0;

        // 4. Update Left Node (the original parent_node)
        parent_node->num_keys = split_idx;
        for (int i = 0; i < split_idx; i++) {
            parent_node->keys[i] = temp_keys[i];
            parent_node->payload.child_pages[i] = temp_children[i];
        }
        parent_node->payload.child_pages[split_idx] = temp_children[split_idx];

        // 5. Update Right Node
        // Notice we start 'j' at split_idx + 1 because the middle key is PROMOTED!
        right_node->num_keys = MAX_KEYS - split_idx;
        for (int i = 0, j = split_idx + 1; i < right_node->num_keys; i++, j++) {
            right_node->keys[i] = temp_keys[j];
            right_node->payload.child_pages[i] = temp_children[j];
        }
        right_node->payload.child_pages[right_node->num_keys] = temp_children[MAX_KEYS + 1];

        // 6. Handle the Promoted Key (Recursive Tree Growth)
        if (parent_node->is_root) {
            // If we split the root, we must create a BRAND NEW root above it!
            uint32_t new_root_page_id = pager->num_pages;
            Page* new_root_page = get_page(pager, new_root_page_id);
            new_root_page->header.page_type = 1; // Index Page
            
            BTreeNode* new_root_node = (BTreeNode*)new_root_page->data;
            new_root_node->is_leaf = 0;
            new_root_node->is_root = 1;
            new_root_node->num_keys = 1;
            new_root_node->keys[0] = promoted_key;
            
            // Point the new root at the left and right nodes
            new_root_node->payload.child_pages[0] = parent_page_id; 
            new_root_node->payload.child_pages[1] = new_right_page_id;

            parent_node->is_root = 0; // Old root is no longer root
            *root_page_id = new_root_page_id; // Update global root tracking

            unpin_page(pager, new_root_page_id, 1);
            unpin_page(pager, new_right_page_id, 1);
            unpin_page(pager, parent_page_id, 1);
            return; 
        } else {
            // Normal internal node split. Push the promoted key UP to the grandparent!
            unpin_page(pager, new_right_page_id, 1);
            unpin_page(pager, parent_page_id, 1);
            
            // NOTE: We must search the tree to find who the parent of parent_page_id is
            uint32_t grand_parent_id = find_parent(pager, *root_page_id, parent_page_id);
            
            // Recursively call this exact function to push it up!
            insert_into_internal(pager, root_page_id, grand_parent_id, promoted_key, new_right_page_id);
            return;
        }
    }

    // ==========================================
    // THE STANDARD INSERT LOGIC (If not full)
    // ==========================================
    int i = 0;
    while (i < parent_node->num_keys && compare_keys(key, parent_node->keys[i]) > 0) i++;

    for (int j = parent_node->num_keys; j > i; j--) {
        parent_node->keys[j] = parent_node->keys[j - 1];
        parent_node->payload.child_pages[j + 1] = parent_node->payload.child_pages[j];
    }

    parent_node->keys[i] = key;
    parent_node->payload.child_pages[i + 1] = right_child_page_id;
    parent_node->num_keys++;

    unpin_page(pager, parent_page_id, 1);
}