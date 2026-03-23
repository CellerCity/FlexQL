#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "btree.h"
#include "pager.h"

// --- Prototypes ---
void split_leaf_node(Pager* pager, uint32_t* root_page_id, Page* old_page, IndexKey new_key, RecordID new_record);
void create_new_root(Pager* pager, uint32_t* root_page_id, uint32_t left_page_id, uint32_t right_page_id, IndexKey key);
void insert_into_internal(Pager* pager, uint32_t* root_page_id, uint32_t parent_page_id, IndexKey key, uint32_t right_child_page_id);

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

        int i = 0;
        while (i < node->num_keys && compare_keys(search_key, node->keys[i]) > 0) i++;

        if (node->is_leaf) {
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
        int i = 0;
        while (i < node->num_keys && compare_keys(key, node->keys[i]) >= 0) i++;
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
        insert_into_internal(pager, root_page_id, old_node->parent_page_id, middle_key, new_page_id);
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

void insert_into_internal(Pager* pager, uint32_t* root_page_id, uint32_t parent_page_id, IndexKey key, uint32_t right_child_page_id) {
    Page* parent_page = get_page(pager, parent_page_id);
    BTreeNode* parent_node = (BTreeNode*)parent_page->data;

    if (parent_node->num_keys >= MAX_KEYS) {
        printf("[-] FATAL: Internal node split not yet implemented.\n");
        exit(EXIT_FAILURE);
    }

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