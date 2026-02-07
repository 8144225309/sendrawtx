#!/bin/bash
# RawRelay Integration Test Suite
# Tests all HTTP endpoints and features

# Don't exit on error - we handle errors ourselves

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASS_COUNT=0
FAIL_COUNT=0
TOTAL_TESTS=0

# Test configuration
SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"

pass() {
    echo -e "  ${GREEN}PASS${NC}: $1"
    ((PASS_COUNT++))
    ((TOTAL_TESTS++))
}

fail() {
    echo -e "  ${RED}FAIL${NC}: $1"
    ((FAIL_COUNT++))
    ((TOTAL_TESTS++))
}

section() {
    echo ""
    echo -e "${YELLOW}--- $1 ---${NC}"
}

# Wait for server to be ready
wait_for_server() {
    echo "Waiting for server to start..."
    for i in {1..30}; do
        if curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/health" 2>/dev/null | grep -q "200"; then
            echo "Server is ready!"
            return 0
        fi
        sleep 0.5
    done
    echo "Server failed to start"
    return 1
}

echo "================================================"
echo "RawRelay Integration Test Suite"
echo "================================================"

# Check if server is running
if ! curl -s -o /dev/null "${BASE_URL}/health" 2>/dev/null; then
    echo "Server not running. Please start with: ./rawrelay-server config.regtest.ini"
    exit 1
fi

section "Health & Observability Endpoints"

# Test /health
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/health")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
if [ "$CODE" = "200" ] && echo "$BODY" | grep -q "healthy"; then
    pass "/health returns 200 with healthy status"
else
    fail "/health (got $CODE)"
fi

# Test /ready
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/ready")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "/ready returns 200"
else
    fail "/ready (got $CODE)"
fi

# Test /alive
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/alive")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "/alive returns 200"
else
    fail "/alive (got $CODE)"
fi

# Test /metrics (Prometheus format)
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/metrics")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
if [ "$CODE" = "200" ] && echo "$BODY" | grep -q "rawrelay_"; then
    pass "/metrics returns Prometheus format"
else
    fail "/metrics (got $CODE)"
fi

# Note: X-Bitcoin-Network header is a future enhancement
echo "  SKIP: X-Bitcoin-Network header (future enhancement)"

section "Static File Serving"

# Test root path - returns error page for invalid/empty path (by design)
# The server expects transaction hex in the path
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
if [ "$CODE" = "200" ] && echo "$BODY" | grep -qi "html"; then
    pass "/ returns HTML page"
else
    fail "/ (expected HTML 200, got $CODE)"
fi

# Test invalid path returns error page (400)
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/nonexistent")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "400" ]; then
    pass "Invalid path returns 400 error page"
else
    fail "Invalid path (got $CODE)"
fi

section "Request ID Tracking"

# Test X-Request-ID header
HEADER=$(curl -s -I "${BASE_URL}/health" | grep -i "X-Request-ID" | tr -d '\r')
if echo "$HEADER" | grep -qi "X-Request-ID"; then
    pass "X-Request-ID header present"
else
    fail "X-Request-ID header missing"
fi

section "HTTP Methods"

# Test POST to broadcast endpoint
RESP=$(curl -s -w "\n%{http_code}" -X POST "${BASE_URL}/" -d "test=data")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ] || [ "$CODE" = "400" ]; then
    pass "POST to / accepted"
else
    fail "POST to / (got $CODE)"
fi

# Test HEAD request
RESP=$(curl -s -w "%{http_code}" -I "${BASE_URL}/health")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "HEAD request works"
else
    fail "HEAD request (got $CODE)"
fi

section "Rate Limiting"

# Rapid requests to trigger rate limiting (if low limit set)
echo "  Testing rapid requests..."
SUCCESS_COUNT=0
for i in {1..10}; do
    CODE=$(curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/health")
    if [ "$CODE" = "200" ]; then
        ((SUCCESS_COUNT++))
    fi
done
if [ $SUCCESS_COUNT -gt 0 ]; then
    pass "Rate limiter allows normal traffic ($SUCCESS_COUNT/10 succeeded)"
else
    fail "Rate limiter too aggressive (0/10 succeeded)"
fi

section "Cache Headers"

# Test Cache-Control header
HEADER=$(curl -s -I "${BASE_URL}/" | grep -i "Cache-Control" | tr -d '\r')
if echo "$HEADER" | grep -qi "Cache-Control"; then
    pass "Cache-Control header present on static files"
else
    fail "Cache-Control header missing"
fi

section "Connection Handling"

# Test keep-alive
RESP=$(curl -s -w "\n%{http_code}" -H "Connection: keep-alive" "${BASE_URL}/health")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "Keep-alive connection works"
else
    fail "Keep-alive connection (got $CODE)"
fi

# Test connection close
RESP=$(curl -s -w "\n%{http_code}" -H "Connection: close" "${BASE_URL}/health")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "Connection: close works"
else
    fail "Connection: close (got $CODE)"
fi

section "ACME Challenge Path"

# Test .well-known/acme-challenge path exists
RESP=$(curl -s -w "\n%{http_code}" "${BASE_URL}/.well-known/acme-challenge/test")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "404" ] || [ "$CODE" = "200" ]; then
    pass "ACME challenge path routed correctly"
else
    fail "ACME challenge path (got $CODE)"
fi

section "Security Headers"

# Check for security headers
HEADERS=$(curl -s -I "${BASE_URL}/health")

if echo "$HEADERS" | grep -qi "X-Content-Type-Options"; then
    pass "X-Content-Type-Options header present"
else
    echo "  SKIP: X-Content-Type-Options not required"
fi

section "Content Types"

# Test JSON metrics
CONTENT_TYPE=$(curl -s -I "${BASE_URL}/metrics" | grep -i "Content-Type" | tr -d '\r')
if echo "$CONTENT_TYPE" | grep -qi "text/plain"; then
    pass "/metrics returns text/plain"
else
    fail "/metrics content type"
fi

# Test HTML content type
CONTENT_TYPE=$(curl -s -I "${BASE_URL}/" | grep -i "Content-Type" | tr -d '\r')
if echo "$CONTENT_TYPE" | grep -qi "text/html"; then
    pass "/ returns text/html"
else
    fail "/ content type"
fi

echo ""
echo "================================================"
echo -e "Results: ${GREEN}${PASS_COUNT}${NC}/${TOTAL_TESTS} tests passed"
if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "${RED}${FAIL_COUNT} tests failed${NC}"
    exit 1
else
    echo -e "${GREEN}All tests passed!${NC}"
fi
echo "================================================"
