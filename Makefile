# ==========================================
# FlexQL Compiler Definitions
# ==========================================
CC = gcc
CXX = g++
CFLAGS = -Wall -lpthread -Wno-format-truncation
CXXFLAGS = -std=c++11 -Wall

# ==========================================
# Target Executables
# ==========================================
SERVER_BIN = flexql-server
CLIENT_BIN = flexql-client
BENCH_BIN = benchmark_flexql
RIGOROUS_BIN = rigorous_tests

# ==========================================
# Build Rules
# ==========================================

# The default 'make' command builds everything and makes scripts executable
all: server client benchmark tests scripts

server:
	@echo "[*] Compiling FlexQL Server..."
	$(CC) src/network/server.c src/storage/executor.c src/storage/schema.c src/storage/btree.c src/storage/pager.c src/parser/parser.c -o $(SERVER_BIN) $(CFLAGS)

client:
	@echo "[*] Compiling FlexQL Interactive REPL..."
	$(CC) src/network/client.c src/client/flexql.c -o $(CLIENT_BIN)

benchmark:
	@echo "[*] Compiling FlexQL Benchmark Suite..."
	$(CXX) benchmark_flexql.cpp src/client/flexql.c -o $(BENCH_BIN) $(CXXFLAGS)

tests:
	@echo "[*] Compiling Rigorous Semantic Tests..."
	$(CXX) tests/rigorous_tests.cpp src/client/flexql.c -o $(RIGOROUS_BIN) $(CXXFLAGS)

scripts:
	@echo "[*] Setting executable permissions for bash tests..."
	chmod +x tests/concurrent_test.sh tests/test_wal_crash.sh

# ==========================================
# Clean Rule (The Benchmark Reset)
# ==========================================
clean:
	@echo "[*] Cleaning up executables and logs..."
	rm -f $(SERVER_BIN) $(CLIENT_BIN) $(BENCH_BIN) $(RIGOROUS_BIN) client1_log.txt client2_log.txt
	@echo "[*] Wiping database directory (flexql_data)..."
	rm -rf flexql_data
	@echo "[+] Clean complete! Ready for fresh benchmarks."
	