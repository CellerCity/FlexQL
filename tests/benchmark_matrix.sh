#!/bin/bash

# Ensure the script is run from the root directory
if [ ! -f "Makefile" ]; then
    echo "[-] Error: Please run this script from the root project directory."
    echo "    Command: ./tests/benchmark_matrix.sh"
    exit 1
fi

mkdir -p benchmark_logs

echo "=================================================="
echo "      FlexQL Hardware Profiling Automator"
echo "=================================================="

# 1. Get the current hardware state from the user
echo "Select Action:"
echo "1) Run Benchmark: Plugged In (AC)"
echo "2) Run Benchmark: Battery (DC)"
echo "3) Scrape Logs & Generate Markdown Table ONLY"
read -p "> " action_choice

if [ "$action_choice" == "3" ]; then
    goto_scraper=true
else
    goto_scraper=false
    if [ "$action_choice" == "1" ]; then POWER="Plugged In"; else POWER="Battery"; fi
    
    echo ""
    echo "Select your current OS Power Mode:"
    echo "1) Performance"
    echo "2) Balanced"
    echo "3) Power Saver"
    read -p "> " mode_choice

    if [ "$mode_choice" == "1" ]; then MODE="Performance"
    elif [ "$mode_choice" == "2" ]; then MODE="Balanced"
    else MODE="Power Saver"; fi

    LOG_FILE="benchmark_logs/benchmark_${POWER// /_}_${MODE// /_}.log"

    echo ""
    echo "[*] Profile selected: $POWER - $MODE"
    echo "[*] Results will be saved to $LOG_FILE"
    echo ""
fi

# --- Helper Function to run a clean benchmark ---
run_benchmark() {
    ROWS=$1
    echo "[*] -----------------------------------------"
    echo "[*] Preparing pristine environment for $ROWS rows..."
    
    # THE GHOST HUNTER
    pkill -9 flexql-server flexql-client benchmark_flexql rigorous_tests 2>/dev/null
    sleep 1 

    make clean > /dev/null 2>&1
    make server benchmark > /dev/null 2>&1

    echo "[*] Starting FlexQL Server in the background..."
    ./flexql-server > /dev/null 2>&1 &
    SERVER_PID=$!
    sleep 2 # Give the server time to bind to port 9000

    echo "[*] Blasting $ROWS rows from the benchmark client..."
    echo "=== RESULTS FOR $ROWS ROWS ===" >> "$LOG_FILE"
    ./benchmark_flexql $ROWS >> "$LOG_FILE" 2>&1
    
    echo "[*] Benchmark complete. Tearing down server..."
    echo "" >> "$LOG_FILE"
    
    # Graceful shutdown to clear WAL
    kill -2 $SERVER_PID
    sleep 1
    kill -9 $SERVER_PID 2>/dev/null
}

# --- The Markdown & Excel Scraper ---
generate_table() {
    echo ""
    echo "[*] Scraping logs from benchmark_logs/..."
    TABLE_FILE="benchmark_logs/FINAL_TABLE.md"
    CSV_FILE="benchmark_logs/FINAL_BENCHMARKS.csv"
    
    # Initialize Markdown and CSV Headers
    echo "### Benchmark Results" > "$TABLE_FILE"
    echo "" >> "$TABLE_FILE"
    echo "Power Source,Power Mode,Row Count,Elapsed Time (ms),Throughput (rows/sec)" > "$CSV_FILE"

    for state in "Battery" "Plugged_In"; do
        disp_state=$(echo $state | tr '_' ' ')
        
        if [ "$state" == "Battery" ]; then 
            echo "#### On battery:" >> "$TABLE_FILE"
        else 
            echo "#### Plugged in:" >> "$TABLE_FILE"
        fi

        for rows in "1000000" "10000000"; do
            if [ "$rows" == "1000000" ]; then 
                echo "##### Stats for 1M records:" >> "$TABLE_FILE"
            else 
                echo "##### Stats for 10M records:" >> "$TABLE_FILE"
            fi

            for mode in "Performance" "Balanced" "Power_Saver"; do
                LOG="benchmark_logs/benchmark_${state}_${mode}.log"
                disp_mode=$(echo $mode | tr '_' ' ')
                
                if [ -f "$LOG" ]; then
                    # Awk magic to strictly pull the numbers we need
                    ELAPSED=$(awk "/=== RESULTS FOR $rows ROWS ===/{flag=1} flag && /Elapsed:/{print \$2; flag=0}" "$LOG")
                    TPUT=$(awk "/=== RESULTS FOR $rows ROWS ===/{flag=1} flag && /Throughput:/{print \$2; flag=0}" "$LOG")
                    
                    if [ -n "$ELAPSED" ]; then
                        # Write to Markdown
                        echo "* $disp_mode mode: **${ELAPSED} ms** (${TPUT} rows/sec)" >> "$TABLE_FILE"
                        # Write to Excel CSV
                        echo "\"$disp_state\",\"$disp_mode\",$rows,$ELAPSED,$TPUT" >> "$CSV_FILE"
                    else
                        echo "* $disp_mode mode: *Incomplete Log*" >> "$TABLE_FILE"
                    fi
                else
                    echo "* $disp_mode mode: *No data yet*" >> "$TABLE_FILE"
                fi
            done
            echo "" >> "$TABLE_FILE"
        done
        
        # Calculate Averages for this Power State (Markdown Only)
        echo "##### Average Throughput (1M & 10M combined):" >> "$TABLE_FILE"
        for mode in "Performance" "Balanced" "Power_Saver"; do
            LOG="benchmark_logs/benchmark_${state}_${mode}.log"
            disp_mode=$(echo $mode | tr '_' ' ')
            if [ -f "$LOG" ]; then
                TPUT1=$(awk "/=== RESULTS FOR 1000000 ROWS ===/{flag=1} flag && /Throughput:/{print \$2; flag=0}" "$LOG")
                TPUT10=$(awk "/=== RESULTS FOR 10000000 ROWS ===/{flag=1} flag && /Throughput:/{print \$2; flag=0}" "$LOG")
                if [[ -n "$TPUT1" && -n "$TPUT10" ]]; then
                    AVG=$(( (TPUT1 + TPUT10) / 2 ))
                    echo "* $disp_mode: **${AVG} rows/sec**" >> "$TABLE_FILE"
                fi
            fi
        done
        echo "" >> "$TABLE_FILE"
    done
    
    echo "[+] Markdown Table successfully generated at: $TABLE_FILE"
    echo "[+] Excel CSV successfully generated at: $CSV_FILE"
    echo "=================================================="
}

# --- Main Execution Flow ---
if [ "$goto_scraper" = true ]; then
    generate_table
else
    # Initialize Log
    echo "# FlexQL Hardware Profile: $POWER | $MODE" > "$LOG_FILE"
    echo "Date: $(date)" >> "$LOG_FILE"
    echo "" >> "$LOG_FILE"

    run_benchmark 1000000
    run_benchmark 10000000

    # Clean up workspace
    pkill -9 flexql-server benchmark_flexql 2>/dev/null
    make clean > /dev/null 2>&1
    
    # Auto-generate the table at the end of the run!
    generate_table
fi