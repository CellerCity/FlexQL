#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pager.h"
#include "btree.h"

int main() {
    printf("==================================================\n");
    printf("   Testing B+ Tree Index & Thread-Safe LRU Pager   \n");
    printf("==================================================\n");

    // 1. Initialize the Pager with a test database file
    // We remove the old file if it exists to start with a clean slate
    remove("test_index.dat"); 
    Pager* pager = pager_open("test_index.dat");
    
    // The root page ID starts at 0, but can change if the root splits!
    uint32_t root_page_id = 0; 

    // 2. Initialize the very first Root Page
    Page* root_page = get_page(pager, root_page_id);
    BTreeNode* root_node = (BTreeNode*)root_page->data;
    
    // Set up the blank slate
    root_node->is_leaf = 1;
    root_node->is_root = 1;
    root_node->num_keys = 0;
    
    // Release the page back to the LRU cache (Marked as dirty so it saves)
    unpin_page(pager, root_page_id, 1); 

    // 3. The Stress Test: Insert 150 records
    // Since MAX_KEYS is 110, this WILL force a split!
    printf("[+] Inserting 150 records (Keys 10, 20, 30... 1500)\n");
    for (int i = 1; i <= 150; i++) {
        IndexKey key;
        key.type = 1; // INT
        key.value.int_val = i * 10; 

        RecordID rec;
        rec.page_num = i + 1000; // Mock data page (e.g., 1001, 1002)
        rec.slot_num = i % 5;    // Mock slot

        btree_insert(pager, &root_page_id, key, rec);
    }
    printf("[+] Inserts complete! New Root Page ID is: %u\n", root_page_id);

    // 4. Search Test A: A key that SHOULD exist
    printf("\n--- Searching for Key 420 ---\n");
    IndexKey search_key_1;
    search_key_1.type = 1;
    search_key_1.value.int_val = 420; 
    RecordID result_1;

    if (btree_search(pager, root_page_id, search_key_1, &result_1)) {
        printf("[SUCCESS] Found Key 420!\n");
        printf("          -> Mapped to Data Page %u, Slot %u\n", result_1.page_num, result_1.slot_num);
        // We inserted i=42 (42*10=420). 
        // Expected Page: 42 + 1000 = 1042. Expected Slot: 42 % 5 = 2.
    } else {
        printf("[FAILED] Could not find Key 420.\n");
    }

    // 5. Search Test B: A key that SHOULD NOT exist
    printf("\n--- Searching for Key 425 ---\n");
    IndexKey search_key_2;
    search_key_2.type = 1;
    search_key_2.value.int_val = 425; 
    RecordID result_2;

    if (btree_search(pager, root_page_id, search_key_2, &result_2)) {
        printf("[FAILED] Wait, found Key 425? It shouldn't be here!\n");
    } else {
        printf("[SUCCESS] Key 425 correctly identified as missing.\n");
    }

    // 6. Cleanup
    pager_close(pager);
    printf("\n[+] Pager closed successfully. Disk flushed.\n");
    printf("==================================================\n");

    return 0;
}


// gcc src/storage/test_btree.c src/storage/pager.c src/storage/btree.c -o test_btree -lpthread
// ./test_btree