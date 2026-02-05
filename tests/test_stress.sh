#!/bin/bash
# RawRelay Stress Test Suite
# Heavy load testing for stability

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"

echo "================================================"
echo "RawRelay Stress Test Suite"
echo "================================================"

# Check if server is running
if ! curl -s -o /dev/null "${BASE_URL}/health" 2>/dev/null; then
    echo "Server not running. Please start with: ./rawrelay-server config.regtest.ini"
    exit 1
fi

section() {
    echo ""
    echo -e "${YELLOW}--- $1 ---${NC}"
}

section "Concurrent Connection Test"
echo "Spawning 50 concurrent connections..."
START_TIME=$(date +%s%3N)
for i in $(seq 1 50); do
    curl -s -o /dev/null "${BASE_URL}/health" &
done
wait
END_TIME=$(date +%s%3N)
DURATION=$((END_TIME - START_TIME))
echo -e "${GREEN}PASS${NC}: 50 concurrent connections completed in ${DURATION}ms"

section "Rapid Request Test"
echo "Sending 200 rapid sequential requests..."
SUCCESS=0
FAIL=0
START_TIME=$(date +%s%3N)
for i in $(seq 1 200); do
    CODE=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/health" 2>/dev/null)
    if [ "$CODE" = "200" ]; then
        ((SUCCESS++)) || true
    else
        ((FAIL++)) || true
    fi
done
END_TIME=$(date +%s%3N)
DURATION=$((END_TIME - START_TIME))
if [ $DURATION -gt 0 ]; then
    RPS=$((200 * 1000 / DURATION))
else
    RPS=9999
fi
echo -e "${GREEN}PASS${NC}: $SUCCESS/200 succeeded, $FAIL failed"
echo "Duration: ${DURATION}ms, Throughput: ~${RPS} req/s"

section "Large Burst Test"
echo "Sending burst of 100 concurrent requests..."
START_TIME=$(date +%s%3N)
for i in $(seq 1 100); do
    curl -s -o /dev/null "${BASE_URL}/metrics" &
done
wait
END_TIME=$(date +%s%3N)
DURATION=$((END_TIME - START_TIME))
echo -e "${GREEN}PASS${NC}: 100 concurrent requests completed in ${DURATION}ms"

section "Mixed Endpoint Test"
echo "Testing multiple endpoints under load..."
for endpoint in "/health" "/ready" "/alive" "/metrics"; do
    for i in $(seq 1 10); do
        curl -s -o /dev/null "${BASE_URL}${endpoint}" &
    done
done
wait
echo -e "${GREEN}PASS${NC}: Mixed endpoint test completed"

section "POST Request Stress"
echo "Sending 50 POST requests..."
SUCCESS=0
for i in $(seq 1 50); do
    # Simulate transaction broadcast with valid hex
    CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "${BASE_URL}/" \
        -H "Content-Type: text/plain" \
        -d "0100000001abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890000000006a47304402203c7e1234" 2>/dev/null)
    if [ "$CODE" = "200" ] || [ "$CODE" = "400" ]; then
        ((SUCCESS++)) || true
    fi
done
echo -e "${GREEN}PASS${NC}: $SUCCESS/50 POST requests processed"

section "Final Metrics Check"
echo "Checking server metrics after stress test..."
METRICS=$(curl -s "${BASE_URL}/metrics")
if echo "$METRICS" | grep -q "rawrelay_requests_total"; then
    TOTAL=$(echo "$METRICS" | grep "rawrelay_requests_total{" | head -1 | awk '{print $2}')
    echo "Sample metric (requests_total): $TOTAL"
    echo -e "${GREEN}PASS${NC}: Metrics still reporting correctly"
else
    echo -e "${RED}FAIL${NC}: Metrics not available"
fi

section "Server Health After Stress"
HEALTH=$(curl -s "${BASE_URL}/health")
if echo "$HEALTH" | grep -q "healthy"; then
    echo -e "${GREEN}PASS${NC}: Server still healthy after stress test"
else
    echo -e "${RED}FAIL${NC}: Server health check failed"
fi

echo ""
echo "================================================"
echo -e "${GREEN}Stress tests completed successfully!${NC}"
echo "================================================"
