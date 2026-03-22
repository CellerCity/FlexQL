#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../parser/parser.h"
#include "pager.h"

// Declare our functions (normally in a header)
uint16_t serialize_row(ParsedQuery* query, ColumnDef* schema, char* tuple_buffer);
int insert_into_page(Page* page, const char* tuple_buffer, uint16_t tuple_size);

int main() {
    printf("--- Testing Storage Serialization & Insertion ---\n");

    // 1. Mock a Schema (This would normally come from your CREATE TABLE cache)
    ColumnDef schema[4];
    strcpy(schema[0].name, "ID");       strcpy(schema[0].type, "INT");
    strcpy(schema[1].name, "PRICE");    strcpy(schema[1].type, "DECIMAL");
    strcpy(schema[2].name, "CREATED");  strcpy(schema[2].type, "DATETIME");
    strcpy(schema[3].name, "NAME");     strcpy(schema[3].type, "VARCHAR");

    // 2. Mock a Parsed INSERT Query
    ParsedQuery query;
    query.type = CMD_INSERT;
    query.value_count = 4;
    strcpy(query.values[0], "42");
    strcpy(query.values[1], "99.95");
    strcpy(query.values[2], "2023-10-31 12:00:00");
    strcpy(query.values[3], "FlexQL_Test");

    // 3. Serialize the row
    char tuple_buffer[MAX_TUPLE_SIZE];
    uint16_t tuple_size = serialize_row(&query, schema, tuple_buffer);
    printf("[+] Serialized row into %d bytes.\n", tuple_size);

    // 4. Create a dummy Page in memory
    Page my_page;
    my_page.header.page_id = 0;
    my_page.header.num_slots = 0;
    my_page.header.free_space_ptr = PAGE_SIZE;

    // 5. Insert the serialized bytes into the page
    int rc = insert_into_page(&my_page, tuple_buffer, tuple_size);
    if (rc == 0) {
        printf("[+] Successfully inserted into page! (Free space remaining: %ld)\n", 
               my_page.header.free_space_ptr - sizeof(PageHeader) - sizeof(Slot));
    }

    // 6. DESERIALIZE IT BACK (The "SELECT" simulation)
    printf("\n--- Reading Data Back from Page ---\n");
    Slot* slots = (Slot*)my_page.data;
    uint16_t data_offset = slots[0].offset - sizeof(PageHeader);
    char* read_ptr = &my_page.data[data_offset];

    // Read Expiration
    TupleHeader read_header;
    memcpy(&read_header, read_ptr, sizeof(TupleHeader));
    read_ptr += sizeof(TupleHeader);
    printf("Expiration Timestamp: %lu\n", read_header.expiration_timestamp);

    // Read INT
    int32_t read_id;
    memcpy(&read_id, read_ptr, sizeof(int32_t));
    read_ptr += sizeof(int32_t);
    printf("ID (INT): %d\n", read_id);

    // Read DECIMAL
    double read_price;
    memcpy(&read_price, read_ptr, sizeof(double));
    read_ptr += sizeof(double);
    printf("Price (DECIMAL): %.2f\n", read_price);

    // Read DATETIME
    int64_t read_time;
    memcpy(&read_time, read_ptr, sizeof(int64_t));
    read_ptr += sizeof(int64_t);
    printf("Created (DATETIME TS): %ld\n", read_time);

    // Read VARCHAR
    uint16_t str_len;
    memcpy(&str_len, read_ptr, sizeof(uint16_t));
    read_ptr += sizeof(uint16_t);
    char read_str[128] = {0};
    memcpy(read_str, read_ptr, str_len);
    printf("Name (VARCHAR): %s\n", read_str);

    return 0;
}


// gcc test_storage.c ../parser/parser.c pager.c executor.c -o test_storage
// ./test_storage.c
