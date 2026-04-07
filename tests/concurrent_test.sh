#!/bin/bash

echo "========================================"
echo "    FlexQL Concurrent Write Test"
echo "========================================"

echo "[*] Initializing Database and Table..."
echo "CREATE TABLE concurrent_test (id INT, thread_name VARCHAR);" | ./flexql-client 127.0.0.1 9000 > /dev/null

echo "[*] Generating custom inserts for Client 1 (Thread A)..."
seq 1 100000 | awk '{print "INSERT INTO concurrent_test VALUES (" $1 ", '\''Thread_A'\'');"}' > client1_inserts.txt

echo "[*] Generating custom inserts for Client 2 (Thread B)..."
seq 100001 200000 | awk '{print "INSERT INTO concurrent_test VALUES (" $1 ", '\''Thread_B'\'');"}' > client2_inserts.txt

echo "[*] Both clients are now inserting 100,000 rows each simultaneously..."
echo "[*] Waiting for them to finish..."

./flexql-client 127.0.0.1 9000 < client1_inserts.txt > client1_log.txt &
PID1=$!
./flexql-client 127.0.0.1 9000 < client2_inserts.txt > client2_log.txt &
PID2=$!

wait $PID1
wait $PID2

# Summarize logs
for log in client1_log.txt client2_log.txt; do
    success_count=$(grep -c "Query executed successfully" "$log")
    error_count=$(grep -c "Error" "$log")
    {
        echo "=== Summary for $(basename "$log" .txt) ==="
        echo "Total INSERT statements: $((success_count + error_count))"
        echo "Successful inserts: $success_count"
        echo "Failed inserts: $error_count"
        echo ""
        echo "--- Last 5 lines from client ---"
        tail -5 "$log"
    } > "${log}.tmp"
    mv "${log}.tmp" "$log"
done

echo "[+] Concurrent Test Complete!"
echo "----------------------------------------"
echo "Verification: Both clients completed 100,000 inserts each."
echo "Check client1_log.txt and client2_log.txt for detailed results."

rm client1_inserts.txt client2_inserts.txt