#!/bin/bash

# Compile the tools
g++ benchmark_flexql.cpp src/client/flexql.c -o benchmark_flexql -std=c++11

echo "[*] Starting Concurrent Write Test..."

# Run Client 1 in the background (Inserting 200,000 rows)
./benchmark_flexql 200000 > client1_log.txt &
PID1=$!

# Run Client 2 in the background (Inserting 200,000 rows simultaneously)
./benchmark_flexql 200000 > client2_log.txt &
PID2=$!

echo "[*] Both clients are now inserting into the server simultaneously!"
echo "[*] Waiting for them to finish..."

# Wait for both background processes to complete
wait $PID1
wait $PID2

echo "[+] Concurrent Test Complete!"
echo "Check client1_log.txt and client2_log.txt to ensure both succeeded without crashing."