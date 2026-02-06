#!/bin/bash
#
# A/B Performance Test for PR #2 (perf-optimizations)
#
# This script compares performance between main branch and perf-optimizations branch.
# Works on Linux (WSL) and macOS.
#
# Requirements:
#   - ab (Apache Bench) OR wrk
#   - curl
#   - bc
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="$REPO_DIR/ab_test_results"
PORT=8080
REQUESTS=2000
CONCURRENCY=50

mkdir -p "$RESULTS_DIR"

echo -e "${BLUE}=======================================${NC}"
echo -e "${BLUE}  Performance A/B Test for PR #2${NC}"
echo -e "${BLUE}=======================================${NC}"
echo ""
echo "Repository: $REPO_DIR"
echo "Results: $RESULTS_DIR"
echo ""

# Detect benchmark tool
if command -v wrk &> /dev/null; then
    BENCH_TOOL="wrk"
    echo "Using: wrk"
elif command -v ab &> /dev/null; then
    BENCH_TOOL="ab"
    echo "Using: ab (Apache Bench)"
else
    echo -e "${RED}ERROR: Neither wrk nor ab found${NC}"
    echo "Install with: brew install wrk (macOS) or apt install apache2-utils (Linux)"
    exit 1
fi
echo ""

cleanup() {
    pkill -f rawrelay-server 2>/dev/null || true
    sleep 1
}

build_branch() {
    local branch="$1"
    local label="$2"

    echo -e "${YELLOW}Building $label ($branch)...${NC}"
    cd "$REPO_DIR"
    git checkout "$branch" --quiet 2>/dev/null || git checkout "$branch"
    make clean > /dev/null 2>&1 || true

    if ! make -j4 > "$RESULTS_DIR/$label-build.log" 2>&1; then
        echo -e "${RED}Build failed${NC}"
        cat "$RESULTS_DIR/$label-build.log"
        exit 1
    fi

    cp rawrelay-server "$RESULTS_DIR/rawrelay-server-$label"
    echo -e "${GREEN}Built: $label${NC}"
}

start_server() {
    local binary="$1"
    local label="$2"

    cleanup
    "$binary" -w 2 > "$RESULTS_DIR/$label-server.log" 2>&1 &
    SERVER_PID=$!

    for i in {1..20}; do
        if curl -s "http://127.0.0.1:$PORT/health" > /dev/null 2>&1; then
            echo "Server ready (PID: $SERVER_PID)"
            return 0
        fi
        sleep 0.3
    done

    echo -e "${RED}Server failed to start${NC}"
    cat "$RESULTS_DIR/$label-server.log" | tail -20
    exit 1
}

# Run benchmark and extract requests/sec
run_bench() {
    local url="$1"
    local outfile="$2"

    if [ "$BENCH_TOOL" = "wrk" ]; then
        wrk -t2 -c$CONCURRENCY -d10s "$url" 2>&1 | tee "$outfile"
        grep "Requests/sec" "$outfile" | awk '{print $2}'
    else
        ab -n $REQUESTS -c $CONCURRENCY -q "$url" 2>&1 | tee "$outfile"
        grep "Requests per second" "$outfile" | awk '{print $4}'
    fi
}

run_test_suite() {
    local binary="$1"
    local label="$2"

    echo ""
    echo -e "${BLUE}=== Testing: $label ===${NC}"

    start_server "$binary" "$label"

    # Warmup
    echo "Warming up..."
    for i in {1..50}; do
        curl -s "http://127.0.0.1:$PORT/health" > /dev/null
    done

    # Test 1: Health (baseline - no hex validation)
    echo ""
    echo -e "${YELLOW}Test 1: /health (baseline)${NC}"
    local rps1=$(run_bench "http://127.0.0.1:$PORT/health" "$RESULTS_DIR/$label-health.txt")
    echo "$rps1" > "$RESULTS_DIR/$label-health-rps.txt"
    echo "  Result: $rps1 req/s"

    # Test 2: 64-char hex (txid) - tests hex lookup table
    echo ""
    echo -e "${YELLOW}Test 2: /txid (64-char hex)${NC}"
    local txid="0000000000000000000000000000000000000000000000000000000000000000"
    local rps2=$(run_bench "http://127.0.0.1:$PORT/$txid" "$RESULTS_DIR/$label-txid.txt")
    echo "$rps2" > "$RESULTS_DIR/$label-txid-rps.txt"
    echo "  Result: $rps2 req/s"

    # Test 3: 200-char hex (rawtx) - more hex validation
    echo ""
    echo -e "${YELLOW}Test 3: /rawtx (200-char hex)${NC}"
    local hex200=$(printf '0%.0s' {1..200})
    local rps3=$(run_bench "http://127.0.0.1:$PORT/$hex200" "$RESULTS_DIR/$label-rawtx.txt")
    echo "$rps3" > "$RESULTS_DIR/$label-rawtx-rps.txt"
    echo "  Result: $rps3 req/s"

    # Test 4: 1000-char hex (large) - tests chunk buffer
    echo ""
    echo -e "${YELLOW}Test 4: /large (1000-char hex)${NC}"
    local hex1000=$(printf 'a%.0s' {1..1000})
    local rps4=$(run_bench "http://127.0.0.1:$PORT/$hex1000" "$RESULTS_DIR/$label-large.txt")
    echo "$rps4" > "$RESULTS_DIR/$label-large-rps.txt"
    echo "  Result: $rps4 req/s"

    # Memory measurement
    echo ""
    echo -e "${YELLOW}Memory usage:${NC}"
    local pid=$(pgrep -f "rawrelay-server" | head -1)
    if [ -n "$pid" ]; then
        local mem=$(ps -o rss= -p $pid 2>/dev/null | tr -d ' ')
        echo "$mem" > "$RESULTS_DIR/$label-memory.txt"
        echo "  RSS: ${mem} KB"
    fi

    cleanup
}

compare_results() {
    echo ""
    echo -e "${BLUE}=======================================${NC}"
    echo -e "${BLUE}  RESULTS COMPARISON${NC}"
    echo -e "${BLUE}=======================================${NC}"
    echo ""

    printf "%-20s %12s %12s %10s\n" "Test" "Baseline" "Optimized" "Change"
    printf "%-20s %12s %12s %10s\n" "----" "--------" "---------" "------"

    local dominated=0
    local dominated_tests=""

    for test in health txid rawtx large; do
        local base=$(cat "$RESULTS_DIR/baseline-$test-rps.txt" 2>/dev/null)
        local opt=$(cat "$RESULTS_DIR/optimized-$test-rps.txt" 2>/dev/null)

        if [ -n "$base" ] && [ -n "$opt" ]; then
            # Handle decimal numbers properly
            local change=$(echo "scale=1; (($opt - $base) / $base) * 100" | bc 2>/dev/null || echo "0")

            local color="$YELLOW"
            local sign=""
            if (( $(echo "$change > 5" | bc -l) )); then
                color="$GREEN"
                sign="+"
                dominated=$((dominated + 1))
                dominated_tests="$dominated_tests $test"
            elif (( $(echo "$change < -5" | bc -l) )); then
                color="$RED"
            else
                sign="+"
            fi

            printf "%-20s %10.1f %10.1f %b%s%.1f%%%b\n" "$test" "$base" "$opt" "$color" "$sign" "$change" "$NC"
        fi
    done

    # Memory comparison
    local base_mem=$(cat "$RESULTS_DIR/baseline-memory.txt" 2>/dev/null)
    local opt_mem=$(cat "$RESULTS_DIR/optimized-memory.txt" 2>/dev/null)
    if [ -n "$base_mem" ] && [ -n "$opt_mem" ]; then
        local mem_change=$(echo "scale=1; (($opt_mem - $base_mem) / $base_mem) * 100" | bc 2>/dev/null || echo "0")
        printf "%-20s %10s KB %10s KB " "Memory" "$base_mem" "$opt_mem"
        if (( $(echo "$mem_change <= 0" | bc -l) )); then
            echo -e "${GREEN}${mem_change}%${NC}"
        else
            echo -e "${YELLOW}+${mem_change}%${NC}"
        fi
    fi

    echo ""
    echo -e "${BLUE}=======================================${NC}"

    # Verdict
    if [ $dominated -ge 2 ]; then
        echo -e "${GREEN}  PASS: Optimizations verified ($dominated tests improved)${NC}"
        echo -e "${GREEN}  Improved:$dominated_tests${NC}"
    elif [ $dominated -ge 1 ]; then
        echo -e "${YELLOW}  PARTIAL: Only $dominated test(s) improved${NC}"
    else
        echo -e "${RED}  FAIL: No measurable improvement${NC}"
    fi

    echo -e "${BLUE}=======================================${NC}"
}

main() {
    cd "$REPO_DIR"
    local original_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")

    cleanup

    echo -e "${BLUE}=== PHASE 1: Build ===${NC}"
    build_branch "main" "baseline"
    build_branch "perf-optimizations" "optimized"

    git checkout "$original_branch" --quiet 2>/dev/null || true

    echo ""
    echo -e "${BLUE}=== PHASE 2: Benchmark ===${NC}"
    run_test_suite "$RESULTS_DIR/rawrelay-server-baseline" "baseline"
    run_test_suite "$RESULTS_DIR/rawrelay-server-optimized" "optimized"

    echo ""
    echo -e "${BLUE}=== PHASE 3: Compare ===${NC}"
    compare_results

    echo ""
    echo "Full results saved to: $RESULTS_DIR/"
}

trap cleanup EXIT
main "$@"
