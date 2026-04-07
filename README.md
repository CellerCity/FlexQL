# FlexQL: A High-Performance Relational Database Engine

**GitHub Repository:** https://github.com/CellerCity/FlexQL

FlexQL is a custom-built, highly optimized relational database management system written entirely in C/C++. It features a multithreaded client-server architecture, supports standard SQL operations, bulk inserts, B+ Tree indexing, an LRU Buffer Pool, thread-safe concurrency, and fault-tolerant crash recovery.

## 1. Compilation and Execution Instructions

### Compile the System
To compile the server, the REPL client, and the benchmark suites, simply run:
`make`

*(To reset the database and clear all old data for a fresh benchmark, run `make clean`)*


### Running the System
1. **Start the server first.** It will automatically initialize the data directory (`flexql_data`) and boot up the recovery engine:
   ```bash
   ./flexql-server
   ```
2. **Start the REPL Client:** (In a separate terminal)
   ```bash
   ./flexql-client 127.0.0.1 9000
   ```
3. **Run the Unit Tests:**
   ```bash
   ./benchmark_flexql --unit-test
   ```
4. **Run the Performance Benchmark** (Defaults to 1,000,000 rows):
   ```bash
   ./benchmark_flexql 1000000
   ```


### Running the Custom Test Suite
**1. The Semantic Integrity Test (Rigorous Constraints):**
```bash
./rigorous_tests
```

**2. The Thread Safety Test (Concurrent Writes):**
```bash
./tests/concurrent_test.sh
```

**3. The Fault Tolerance Test (WAL Crash & Recovery):**
```bash
./tests/test_wal_crash.sh
```

---



## 2. Directory Structure

Our codebase is organized to enforce a strict separation of concerns:
* **`src/network/`** (`server.c`, `client.c`): Handles TCP sockets, multithreading (`pthreads`), and receiving massive data streams safely.
* **`src/parser/`** (`parser.c`, `parser.h`): Validates raw SQL strings, checks for semantic errors, and converts them into an internal `ParsedQuery` struct.
* **`src/storage/`**: The core execution engine.
  * `executor.c`: Handles query routing, constraint checking (NOT NULL, type safety), and executing Joins.
  * `pager.c`: Manages reading/writing 4KB pages to the hard drive (`.dat` files) using an LRU Buffer Pool.
  * `btree.c`: Maintains the B+ Tree index.
  * `schema.c`: Saves and loads table blueprints (data types and column names).
* **`src/client/`** (`flexql.c`): The C driver API used by external applications to connect to the database.

---



## 2.1 Example Usage – REPL Client

After starting the server and client, we can execute SQL‑like commands. The system includes a `default_db` database that is used automatically if no database is selected.

### Basic Commands

```sql
-- Create a new database
CREATE DATABASE school;

-- Switch to a database
USE school;

-- Create two tables
CREATE TABLE students (id INT, name VARCHAR, grade DECIMAL);
CREATE TABLE courses (student_id INT, course_name VARCHAR, score INT);

INSERT INTO students VALUES (1, 'Alice', 85.5);
INSERT INTO students VALUES (2, 'Bob', 92.0);
INSERT INTO students VALUES (3, 'Carol', 78.0);

INSERT INTO courses VALUES (1, 'Math', 90);
INSERT INTO courses VALUES (1, 'Physics', 85);
INSERT INTO courses VALUES (2, 'Math', 95);
INSERT INTO courses VALUES (3, 'Chemistry', 70);
```

### SELECT with WHERE and Comparison Operators

You can use `=`, `>`, `>=`, `<`, `<=`, `<>` in the `WHERE` clause.

```sql
-- Students with grade >= 80
SELECT * FROM students WHERE grade >= 80;
```
**Expected output:**
```
id = 1
name = Alice
grade = 85.5

id = 2
name = Bob
grade = 92.0
```

```sql
-- Students with grade < 80
SELECT name FROM students WHERE grade < 80;
```
**Expected output:**
```
name = Carol
```

```sql
-- Students with id <= 2
SELECT id, name FROM students WHERE id <= 2;
```
**Expected output:**
```
id = 1
name = Alice

id = 2
name = Bob
```


### INNER JOIN with WHERE Clause

```sql
-- Join with filter on the joined table
SELECT * FROM students
INNER JOIN courses ON students.id = courses.student_id
WHERE courses.score > 85;
```
**Expected output:**
```

id = 1
name = Alice grade = 85.5 
student_id = 1
course_name = Math 
score = 90

id = 1
name = Alice grade = 85.5 
student_id = 1
course_name = Physics 
score = 85

id = 2
name = Bob grade = 92.0 
student_id = 2
course_name = Math 
score = 95
```

### Working Without a Selected Database

If you never issue `USE <dbname>`, all operations run inside the `default_db` database, which is created automatically.

```sql
-- These commands work even without USE
CREATE TABLE test (id INT);
INSERT INTO test VALUES (10);
SELECT * FROM test;
```

### Other Useful Commands

```sql
-- Show all databases
SHOW DATABASES;

-- Show tables in current database
SHOW TABLES;

-- Drop a table
DROP TABLE students;

-- Exit the client
.exit
```

### Important Notes

- Primary keys are automatically indexed using a B+ Tree.
- The client supports batched inserts for high throughput (with a batch size upto 5000 per insert statement).

---




## 3. Core Design Decisions

### 3.1 How the Data is Stored
Data is stored sequentially in 4KB blocks using a **Row-Major Slotted Page** architecture. Row-major ensures entire records are written sequentially, minimizing CPU pointer-jumping and maximizing insertion throughput.
* **Data Types & The VARCHAR Optimization:** We avoid heap allocation (`malloc`/`free`) per row. In RAM, strings are parsed into fixed-size stack arrays for speed. However, during disk serialization, `VARCHAR` fields are dynamically packed (storing a 2-byte length prefix followed strictly by the character bytes). This provides the execution speed of fixed-size arrays in memory while perfectly preserving hard drive space.
  * `INT` & `DECIMAL`: 4 bytes and 8 bytes respectively.
  * `DATETIME`: 8 bytes (Stored internally as a Unix epoch timestamp `int64_t` for highly efficient `<` and `>` comparisons).


### 3.2 Indexing Method
Scanning 10 million rows to find a single ID is too slow. For tables with a Primary Key, I implemented a **B+ Tree**. 
* The tree lives directly inside our 4KB pages on the hard drive. 
* It maps an `IndexKey` to a `RecordID` (which is simply a `page_num` and a `slot_num`). This allows our database to find any row instantly in just 3 or 4 disk jumps, supporting searches across Integers, Decimals, Datetimes, and Varchars.

### 3.3 Caching Strategy
The assignment requires a caching mechanism to speed up repeated queries. I implemented two layers of caching:
1. **LRU Buffer Pool (The Pager):** Reading from a hard drive is painfully slow. I built a RAM cache that holds the most frequently accessed 4KB pages. I use a **Least Recently Used (LRU)** eviction policy. If the memory fills up, our Doubly-Linked List safely kicks out the oldest unpinned page to make room. 
2. **Thread-Local Schema Caching:** For every query, the engine needs to know the column data types. Instead of reading the `.schema` file from the disk every time, the thread loads it into RAM once and caches it. This completely bypassed massive disk I/O bottlenecks.

### 3.4 Handling of Expiration Timestamps
The assignment requires each inserted row to have an expiration timestamp. I implemented a **Lazy Deletion** strategy.
* **Why?** Constantly running background threads to delete old rows would destroy our insertion throughput. 
* **How it works:** When a row is inserted, the timestamp is attached to its `TupleHeader`. When a user runs a `SELECT` or `JOIN` query, the engine checks the timestamp. If the row is expired, the engine silently skips it. This keeps inserts at $O(1)$ time complexity while still honoring the expiration rules. 

### 3.5 Multithreading Design
The server handles multiple clients simultaneously. To prevent memory corruption (e.g., two clients appending to the same page at the same time), I implemented a **Table-Level Reader-Writer Lock Manager** (`pthread_rwlock_t`).
* **Readers:** Multiple `SELECT` queries can scan the same table simultaneously without waiting.
* **Writers:** An `INSERT` query grabs an exclusive lock, safely updates the B-Tree, and releases it in microseconds.
* **Trade-off:** I chose table-level locking over row-level locking because `INSERT` operations strictly append to the end of the file. Row-level locks would still cause massive contention on the active page's "free space pointer." Table locks give us absolute safety while maintaining extreme throughput.

---

## 4. Advanced Features & Performance Optimizations

Getting our database to process hundreds of thousands of rows per second required us to overcome several major bottlenecks.

* **Network Overhead & TCP Batching:** Originally, sending a TCP packet for every single row caused massive network latency. I implemented TCP Batching. The client sends up to 5,000 rows in a single formatted string. The server parses the entire string in memory and commits it, drastically cutting insertion times.
* **The 100-Column Limit (Memory Fragmentation):** Using `malloc` and `free` for every column of every row caused severe memory fragmentation. I enforced a deliberate **100-column hard limit** per table. This allowed us to use static stack arrays instead of dynamically allocating heap memory. This $O(1)$ memory allocation strategy ensures perfect data locality in the CPU cache.
* **Fault Tolerance (Write-Ahead Logging):** To survive power outages, I implemented a Group-Commit WAL. Before touching the disk, I validate batches in RAM. If they pass, I log the string to `recovery.wal`. If the server crashes mid-batch, the next boot reads the WAL and uses **Idempotent Replay**—silently skipping rows already in the B-Tree and cleanly finishing the rest of the batch.

---

## 5. The Smart Join Engine
I built an intelligent execution engine for `INNER JOIN` operations that dynamically analyzes schemas. 
* If **Table A** has an index but **Table B** does not, the engine scans Table B sequentially and fires fast $O(\log N)$ binary searches into Table A's B+ Tree. 
* It automatically flips the inner and outer loops based on which table is indexed to guarantee the fastest path. 
* If neither is indexed, it gracefully falls back to a Block Nested Loop Join.

---

## 6. Custom Test Suites & Fault Tolerance Validation

To ensure production-grade stability, I developed a suite of automated stress tests located in the `tests/` directory:

1. **`test_wal_crash.sh` (Fault Tolerance):**
   FlexQL utilizes a Group-Commit Write-Ahead Log (WAL). This script blasts a batch of inserts to the server and intentionally executes a lethal `kill -9` termination mid-flight. On the next boot, the server reads `recovery.wal`, uses **Idempotent Replay** to safely skip rows already in the B-Tree, and successfully finishes the partial batch. This guarantees strict Atomicity.
2. **`concurrent_test.sh` (Thread Safety):**
   Spawns multiple background clients that simultaneously blast hundreds of thousands of inserts into the same server port to validate that our `pthread_rwlock` manager prevents B-Tree memory corruption.
3. **`rigorous_tests.cpp` (Semantic Integrity):**
   A C++ suite that intentionally fires malformed queries at the server to validate our Pre-Flight checks (e.g., rejecting duplicate primary keys, rejecting strings inside `INT` columns, and enforcing `NOT NULL` constraints).



***


### 7. Automated Hardware Profiling & Power State Variance

To ensure our benchmarking methodology is completely standardized and reproducible, we built an automated profiling tool (`tests/benchmark_matrix.sh`). 

**The Profiling Matrix:**
This script automatically cycles the database through a matrix of hardware states (Plugged-In vs. Battery) and OS CPU Governors (Performance, Balanced, Power Saver). For each state, it wipes the data directory, boots the server, blasts 1M and 10M rows, kills the server, and logs the output. Finally, an embedded `awk` scraper parses the raw logs and automatically generates a formatted Markdown table of our throughput metrics.

**The "Why" Behind Power State Variance:**
During profiling, we observed a massive 2x performance drop when running the benchmarks on battery power versus wall power. This is caused by the operating system's Advanced Configuration and Power Interface (ACPI) policies. 
* On battery power, the Linux kernel aggressively switches the CPU governor from `performance` to `powersave`. 
* This disables CPU Turbo Boost, heavily downclocks core frequencies, and throttles hard-drive I/O to conserve wattage. 
* Because FlexQL processes data at such high velocity, it immediately bottlenecks against these artificial OS limits. For maximum throughput, the host machine must be on wall power with the `performance` governor explicitly enabled.

**📥 Raw Data Export:** [Download FINAL_BENCHMARKS.csv](benchmark_logs/FINAL_BENCHMARKS.csv)


### Benchmark Results

#### On battery:
##### Stats for 1M records:
* Performance mode: **1086 ms** (920810 rows/sec)
* Balanced mode: **2074 ms** (482160 rows/sec)
* Power Saver mode: **1739 ms** (575043 rows/sec)

##### Stats for 10M records:
* Performance mode: **9352 ms** (1069289 rows/sec)
* Balanced mode: **19177 ms** (521457 rows/sec)
* Power Saver mode: **21705 ms** (460723 rows/sec)

##### Average Throughput (1M & 10M combined):
* Performance: **995049 rows/sec**
* Balanced: **501808 rows/sec**
* Power Saver: **517883 rows/sec**

#### Plugged in:
##### Stats for 1M records:
* Performance mode: **897 ms** (1114827 rows/sec)
* Balanced mode: **1105 ms** (904977 rows/sec)
* Power Saver mode: **1736 ms** (576036 rows/sec)

##### Stats for 10M records:
* Performance mode: **8847 ms** (1130326 rows/sec)
* Balanced mode: **12768 ms** (783208 rows/sec)
* Power Saver mode: **21053 ms** (474991 rows/sec)

##### Average Throughput (1M & 10M combined):
* Performance: **1122576 rows/sec**
* Balanced: **844092 rows/sec**
* Power Saver: **525513 rows/sec**


***


