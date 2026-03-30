#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include "./src/client/flexql.h"

using namespace std;

// Helper to count rows
struct QueryStats { int rows = 0; };
static int count_rows_callback(void *data, int argc, char **argv, char **azColName) {
    QueryStats *stats = static_cast<QueryStats*>(data);
    if (stats) stats->rows++;
    return 0;
}

// Executes a query and returns TRUE if it SUCCEEDED, FALSE if it threw an ERROR
static bool run_query(FlexQL *db, const string &sql, string &out_error) {
    char *errMsg = nullptr;
    
    // We append a dummy SELECT to force the flexql_exec driver to flush the INSERT pipeline 
    // and wait for a response, otherwise the driver hides INSERT errors!
    string forced_flush_sql = sql;
    if (sql.substr(0, 6) == "INSERT" || sql.substr(0, 6) == "insert") {
        forced_flush_sql += "\nSELECT * FROM dummy_flush;"; 
    }

    int rc = flexql_exec(db, forced_flush_sql.c_str(), nullptr, nullptr, &errMsg);
    
    if (rc != FLEXQL_OK) {
        out_error = errMsg ? errMsg : "unknown error";
        if (errMsg) flexql_free(errMsg);
        return false;
    }
    return true;
}

void print_test(const string& test_name, bool expected_to_fail, bool actually_failed, const string& error_msg) {
    if (expected_to_fail == actually_failed) {
        cout << "[PASS] " << test_name << "\n";
    } else {
        cout << "[FAIL] " << test_name << " -> \n       Expected failure: " 
             << (expected_to_fail ? "YES" : "NO") 
             << ", but got failure: " << (actually_failed ? "YES" : "NO") << "\n"
             << "       Message: " << error_msg << "\n";
    }
}

int main() {
    FlexQL *db = nullptr;
    if (flexql_open("127.0.0.1", 9000, &db) != FLEXQL_OK) {
        cout << "Cannot open FlexQL\n"; return 1;
    }
    cout << "Connected to FlexQL. Running Rigorous Constraints Test...\n\n";

    string err;
    
    // 0. Setup Sandbox
    run_query(db, "CREATE DATABASE strict_db;", err);
    run_query(db, "USE strict_db;", err);

    // ==========================================
    // TEST SUITE 1: DDL & SCHEMA VALIDATION
    // ==========================================
    bool failed = !run_query(db, "CREATE TABLE empty_table ();", err);
    print_test("1. Reject table with no columns", true, failed, err);

    failed = !run_query(db, "DROP TABLE non_existent_table;", err);
    print_test("2. Reject dropping missing table", true, failed, err);

    run_query(db, "CREATE TABLE strict_users (id INT PRIMARY KEY, name VARCHAR, salary DECIMAL, birth DATETIME);", err);

    // ==========================================
    // TEST SUITE 2: PRIMARY KEY & INTEGRITY
    // ==========================================
    run_query(db, "INSERT INTO strict_users VALUES (1, 'Alice', 50000.50, '1990-01-01');", err);
    
    failed = !run_query(db, "INSERT INTO strict_users VALUES (1, 'Bob', 40000.00, '1992-02-02');", err);
    print_test("3. Reject duplicate Primary Key (ID=1)", true, failed, err);

    failed = !run_query(db, "INSERT INTO fake_table VALUES (1, 'Ghost');", err);
    print_test("4. Reject insert into missing table", true, failed, err);

    failed = !run_query(db, "INSERT INTO strict_users VALUES (2, 'Charlie');", err);
    print_test("5. Reject insert with missing columns (2 values, 4 expected)", true, failed, err);

    // ==========================================
    // TEST SUITE 3: TYPE SAFETY (THE CHAOS TESTS)
    // ==========================================
    failed = !run_query(db, "INSERT INTO strict_users VALUES ('not_a_number', 'Dave', 'bad_salary', 'bad_date');", err);
    print_test("6. Reject Invalid Datatypes (Strings into INT/DECIMAL)", true, failed, err);

    failed = !run_query(db, "INSERT INTO strict_users VALUES (3, 'Eve', 100.00, '2002:03:30 00:12:01');", err);
    print_test("7. Reject Invalid Datetime Format (Colons instead of hyphens)", true, failed, err);

    // Verify exactly how much garbage made it into our database
    QueryStats stats;
    flexql_exec(db, "SELECT * FROM strict_users;", count_rows_callback, &stats, nullptr);
    cout << "\n[RESULT] Expected 1 row in 'strict_users'. Actual rows: " << stats.rows << "\n";

    flexql_close(db);
    return 0;
}