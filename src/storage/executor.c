#define _XOPEN_SOURCE // Required for strptime()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "pager.h"
#include "../parser/parser.h" 

uint16_t serialize_row(ParsedQuery* query, ColumnDef* schema, char* tuple_buffer) {
    uint16_t current_offset = 0;

    // 1. Mandatory Expiration Timestamp
    TupleHeader header;
    header.expiration_timestamp = (uint64_t)time(NULL) + (30 * 24 * 60 * 60); 
    
    memcpy(tuple_buffer + current_offset, &header, sizeof(TupleHeader));
    current_offset += sizeof(TupleHeader);

    // 2. Pack the actual column data
    for (int i = 0; i < query->value_count; i++) {
        
        // --- INT (4 bytes) ---
        if (strcasecmp(schema[i].type, "INT") == 0) {
            int32_t val = atoi(query->values[i]);
            memcpy(tuple_buffer + current_offset, &val, sizeof(int32_t));
            current_offset += sizeof(int32_t);
        } 
        // --- DECIMAL (8 bytes) ---
        else if (strcasecmp(schema[i].type, "DECIMAL") == 0) {
            double val = atof(query->values[i]);
            memcpy(tuple_buffer + current_offset, &val, sizeof(double));
            current_offset += sizeof(double);
        }
        // --- DATETIME (8 bytes) ---
        else if (strcasecmp(schema[i].type, "DATETIME") == 0) {
            struct tm tm_info;
            memset(&tm_info, 0, sizeof(struct tm));
            int64_t timestamp = 0;
            
            // Try parsing full datetime, fallback to just date
            if (strptime(query->values[i], "%Y-%m-%d %H:%M:%S", &tm_info) != NULL ||
                strptime(query->values[i], "%Y-%m-%d", &tm_info) != NULL) {
                timestamp = (int64_t)mktime(&tm_info);
            }
            
            memcpy(tuple_buffer + current_offset, &timestamp, sizeof(int64_t));
            current_offset += sizeof(int64_t);
        }
        // --- VARCHAR / TEXT (2 bytes length + N bytes string) ---
        else if (strcasecmp(schema[i].type, "TEXT") == 0 || strcasecmp(schema[i].type, "VARCHAR") == 0) {
            uint16_t len = strlen(query->values[i]);
            memcpy(tuple_buffer + current_offset, &len, sizeof(uint16_t));
            current_offset += sizeof(uint16_t);
            
            memcpy(tuple_buffer + current_offset, query->values[i], len);
            current_offset += len;
        }
    }

    return current_offset; 
}



// Inserts a raw byte array into a specific page.
// Returns 0 on success, or -1 if the page is full.
int insert_into_page(Page* page, const char* tuple_buffer, uint16_t tuple_size) {
    
    // 1. Calculate how much free space we actually have
    // The slots grow top-down, starting right after the PageHeader.
    uint16_t slots_end_offset = sizeof(PageHeader) + (page->header.num_slots * sizeof(Slot));
    
    // Free space is the gap between the last slot and the free_space_ptr
    uint16_t free_space = page->header.free_space_ptr - slots_end_offset;

    // We need enough room for the Tuple Data AND one new Slot
    if (free_space < (tuple_size + sizeof(Slot))) {
        return -1; // PAGE IS FULL! The engine must create a new page.
    }

    // 2. Move the free space pointer up by the size of the tuple
    page->header.free_space_ptr -= tuple_size;

    // 3. Copy the data into the newly claimed space at the bottom of the page
    // Note: Since `free_space_ptr` is an absolute offset from the start of the 4096 block,
    // and our `data` array starts after the PageHeader, we must subtract the header size.
    uint16_t data_array_index = page->header.free_space_ptr - sizeof(PageHeader);
    memcpy(&page->data[data_array_index], tuple_buffer, tuple_size);

    // 4. Create the Slot at the top of the page
    // We treat the very beginning of the `data` array as our array of Slots
    Slot* slot_array = (Slot*)page->data; 
    
    slot_array[page->header.num_slots].offset = page->header.free_space_ptr;
    slot_array[page->header.num_slots].length = tuple_size;

    // 5. Update the header
    page->header.num_slots++;

    return 0; // Success!
}