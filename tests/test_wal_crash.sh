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

# 4. Create the table and build one large single-statement INSERT.
#    200,000 rows takes ~200ms of server-side work end to end (measured),
#    which gives us a wide enough window to land a kill -9 WHILE the insert
#    loop is actually running - not a full second after it already finished,
#    which is what the old 3-row version did.
echo "[*] Creating table..."
echo "CREATE TABLE wal_test (id INT PRIMARY KEY, name VARCHAR);" | nc -q1 127.0.0.1 9000 > /dev/null

BATCH_FILE=$(mktemp /tmp/flexql_wal_batch.XXXXXX.sql)
echo "[*] Generating a 200,000-row single INSERT statement ($BATCH_FILE)..."
python3 -c "
rows = ','.join(f\"({i}, 'Name{i}')\" for i in range(1, 200001))
print('INSERT INTO wal_test VALUES ' + rows + ';')
" > "$BATCH_FILE"

# 5. Send it in the background, then kill mid-flight instead of after it's done.
echo ""
echo "[*] Streaming the batch to the server in the background..."
(cat "$BATCH_FILE"; sleep 5) | nc -q0 127.0.0.1 9000 > /dev/null &
NC_PID=$!

sleep 0.1 # Land inside the WAL-write + execute_insert window, not after it.

echo "[*] 💥 SIMULATING FATAL CRASH (kill -9) MID-BATCH 💥"
kill -9 $SERVER_PID 2>/dev/null
kill -9 $NC_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
sleep 1

# 6. The Recovery Boot
echo ""
echo "[*] Restarting FlexQL Server..."
echo "[*] If WAL works, it should print recovery logs here:"
./flexql-server &
SERVER_PID=$!
sleep 2 # Give the server time to read the WAL and rebuild the B-Tree

# 7. Verification - assert specific rows survived, don't just eyeball a dump.
#    We check point lookups (not a bare SELECT *) scattered across the whole
#    range, including the very last row in the batch, so a replay that quietly
#    stopped partway would get caught.
echo ""
echo "[*] Verifying data survived the crash..."
PASS=1
for ID in 1 50000 100000 150000 199999 200000; do
    RESULT=$(echo "SELECT * FROM wal_test WHERE id = $ID;" | nc -q1 127.0.0.1 9000 | grep -c "^ROW")
    if [ "$RESULT" != "1" ]; then
        echo "[-] MISSING: id=$ID did not survive recovery."
        PASS=0
    else
        echo "[+] Present: id=$ID"
    fi
done

# 8. Clean up the background process
echo "[*] Shutting down test server..."
kill $SERVER_PID 2>/dev/null
rm -f "$BATCH_FILE"

if [ "$PASS" -eq 1 ]; then
    echo "[+] Test Complete: PASS - all sampled rows survived the mid-batch crash."
    exit 0
else
    echo "[-] Test Complete: FAIL - recovery lost rows from a mid-batch crash."
    exit 1
fi
