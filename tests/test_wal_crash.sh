#!/bin/bash

echo "=================================================="
echo "    FlexQL WAL Crash & Recovery Test Suite"
echo "=================================================="

# 0. Kill lingering ghosts!
echo "[*] Cleaning up old background processes..."
pkill -9 flexql-server 2>/dev/null
sleep 1

# 1. Compile the server
echo "[*] Compiling server..."
gcc src/network/server.c src/storage/executor.c src/storage/schema.c src/storage/btree.c src/storage/pager.c src/parser/parser.c -o flexql-server -lpthread

# 2. Wipe old data for a completely clean test
echo "[*] Wiping old data directory..."
rm -rf flexql_data

# 3. Start the server in the background
echo "[*] Starting FlexQL Server..."
./flexql-server &
SERVER_PID=$!
sleep 1 # Wait for server to boot and bind to port 9000

# 4. Send the Batch Insert using Netcat (nc)
echo "[*] Sending Batch Insert (Creating table and inserting 3 rows)..."
# We send the commands and immediately send .exit to close the socket
echo -e "CREATE TABLE wal_test (id INT PRIMARY KEY, name VARCHAR);\nINSERT INTO wal_test VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie');\n.exit\n" | nc 127.0.0.1 9000 > /dev/null

sleep 1 # Give the server a millisecond to process and write to the WAL

# 5. THE CRASH
echo ""
echo "[*] 💥 SIMULATING FATAL POWER LOSS (kill -9) 💥"
kill -9 $SERVER_PID
sleep 1

# 6. The Recovery Boot
echo ""
echo "[*] Restarting FlexQL Server..."
echo "[*] If WAL works, it should print recovery logs here:"
./flexql-server &
SERVER_PID=$!
sleep 2 # Give the server time to read the WAL and rebuild the B-Tree

# 7. Verification
echo ""
echo "[*] Verifying Data Survived the Crash..."
echo -e "SELECT * FROM wal_test;\n.exit\n" | nc 127.0.0.1 9000

# 8. Clean up the background process
echo "[*] Shutting down test server..."
kill $SERVER_PID
echo "[+] Test Complete!"