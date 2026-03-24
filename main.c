#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "src/client/flexql.h" 

int silent_count_callback(void *arg, int columnCount, char **values, char **columnNames) {
    int *row_count = (int *)arg;
    (*row_count)++;
    return 0; 
}

double get_time_in_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char *argv[]) {
    int num_records = 1000; 
    if (argc > 1) {
        num_records = atoi(argv[1]);
        if (num_records > 10000000) num_records = 10000000;
    }

    printf("==================================================\n");
    printf("   FlexQL Performance Benchmark: %d Rows\n", num_records);
    printf("==================================================\n");

    FlexQL *db = NULL;
    char *errmsg = NULL;
    struct timespec start_time, end_time;

    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) {
        printf("[-] Failed to connect to server.\n");
        return 1;
    }

    flexql_exec(db, "CREATE DATABASE bench_db;", NULL, NULL, &errmsg);
    flexql_exec(db, "USE bench_db;", NULL, NULL, &errmsg);
    flexql_exec(db, "CREATE TABLE bench_table (ID INT PRIMARY, DATA VARCHAR);", NULL, NULL, &errmsg);
    
    // --- INSERT BENCHMARK ---
    printf("\n[*] Running INSERT benchmark...\n");
    char insert_query[256];
    int actual_inserts = 0;
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (int i = 1; i <= num_records; i++) {
        snprintf(insert_query, sizeof(insert_query), "INSERT INTO bench_table VALUES (%d, 'TestData');", i);
        if (flexql_exec(db, insert_query, NULL, NULL, &errmsg) != FLEXQL_OK) {
            printf("[-] Insert failed at row %d: %s\n", i, errmsg);
            flexql_free(errmsg);
            break;
        }
        actual_inserts++;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double insert_time = get_time_in_seconds(start_time, end_time);
    
    printf("[+] Inserted %d rows.\n", actual_inserts);
    if (actual_inserts > 0) {
        printf("[+] Total Insert Time : %.4f seconds\n", insert_time);
        printf("[+] Inserts per second: %.0f inserts/sec\n", actual_inserts / insert_time);
    }

    // --- SELECT BENCHMARK ---
    printf("\n[*] Running SELECT (B+ Tree Indexed) benchmark...\n");
    int retrieved_count = 0;
    char select_query[256];
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    // We query specifically by ID to stress-test the B+ Tree traversals
    for (int i = 1; i <= actual_inserts; i++) {
        snprintf(select_query, sizeof(select_query), "SELECT * FROM bench_table WHERE ID = %d;", i);
        if (flexql_exec(db, select_query, silent_count_callback, &retrieved_count, &errmsg) != FLEXQL_OK) {
            printf("[-] Select failed at row %d: %s\n", i, errmsg);
            flexql_free(errmsg);
            break;
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double select_time = get_time_in_seconds(start_time, end_time);
    
    printf("[+] Retrieved %d rows from server.\n", retrieved_count);
    if (retrieved_count > 0) {
        printf("[+] Total Select Time : %.4f seconds\n", select_time);
        printf("[+] Selects per second: %.0f rows/sec\n", retrieved_count / select_time);
    }

    printf("\n[*] Cleaning up database...\n");
    flexql_exec(db, "DROP DATABASE bench_db;", NULL, NULL, &errmsg);
    flexql_close(db);

    return 0;
}