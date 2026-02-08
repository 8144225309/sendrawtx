#!/bin/bash
# RawRelay HTTP/2 Integration Test Suite
# Tests all HTTP/2, TLS, and ALPN features end-to-end
#
# This script is self-contained: it generates temporary TLS certs,
# creates a config file, starts the server, runs tests, and cleans up.
#
# Requirements: curl with HTTP/2 support, openssl CLI

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
HTTP_PORT="18080"
TLS_PORT="18443"
BASE_URL="http://${SERVER_HOST}:${HTTP_PORT}"
TLS_URL="https://${SERVER_HOST}:${TLS_PORT}"
SERVER_PID=""

# Temp directory for certs, config, ACME tokens
TMPDIR=$(mktemp -d /tmp/rawrelay-h2-test.XXXXXX)

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

cleanup() {
    echo ""
    echo "Cleaning up..."
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Generate self-signed TLS certificate
generate_certs() {
    echo "Generating self-signed TLS certificate..."
    openssl req -x509 -newkey rsa:2048 -keyout "$TMPDIR/key.pem" \
        -out "$TMPDIR/cert.pem" -days 1 -nodes \
        -subj "/CN=${SERVER_HOST}" \
        -addext "subjectAltName=IP:${SERVER_HOST}" 2>/dev/null
    if [ $? -ne 0 ]; then
        echo "Failed to generate TLS certificate"
        exit 1
    fi
    echo "Certificate generated."
}

# Create ACME challenge directory and token
setup_acme() {
    ACME_DIR="$TMPDIR/acme-challenges"
    mkdir -p "$ACME_DIR"
    ACME_TOKEN="test-token-abc123"
    ACME_CONTENT="test-token-abc123.thumbprint-xyz789"
    echo -n "$ACME_CONTENT" > "$ACME_DIR/$ACME_TOKEN"
    echo "ACME challenge token created."
}

# Create config file with TLS+HTTP/2 enabled
create_config() {
    cat > "$TMPDIR/config.ini" <<EOF
[network]
chain = regtest

[server]
port = ${HTTP_PORT}
max_connections = 100
read_timeout = 30

[tls]
enabled = 1
port = ${TLS_PORT}
cert_file = ${TMPDIR}/cert.pem
key_file = ${TMPDIR}/key.pem
http2_enabled = 1

[static]
dir = ./static
cache_max_age = 3600

[slots]
normal_max = 100
large_max = 20
huge_max = 5

[ratelimit]
rps = 100.0
burst = 200.0

[acme]
challenge_dir = ${ACME_DIR}

[logging]
json = 0
verbose = 0
EOF
    echo "Config file created."
}

# Start the server
start_server() {
    echo "Starting server..."
    ./rawrelay-server -w 1 "$TMPDIR/config.ini" &
    SERVER_PID=$!
    sleep 0.5

    # Verify it's still running
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "Server failed to start (exited immediately)"
        exit 1
    fi
}

# Wait for server to accept connections on both ports
wait_for_server() {
    echo "Waiting for server to start..."

    # Wait for HTTP port
    for i in {1..30}; do
        if curl -s -o /dev/null -w "%{http_code}" "${BASE_URL}/health" 2>/dev/null | grep -q "200"; then
            echo "HTTP port ready."
            break
        fi
        if [ "$i" = "30" ]; then
            echo "HTTP port failed to respond"
            exit 1
        fi
        sleep 0.5
    done

    # Wait for TLS port
    for i in {1..30}; do
        if curl -sk -o /dev/null -w "%{http_code}" "${TLS_URL}/health" 2>/dev/null | grep -q "200"; then
            echo "TLS port ready."
            break
        fi
        if [ "$i" = "30" ]; then
            echo "TLS port failed to respond"
            exit 1
        fi
        sleep 0.5
    done

    echo "Server is ready!"
}

# ============================================================
# Setup
# ============================================================

echo "================================================"
echo "RawRelay HTTP/2 Integration Test Suite"
echo "================================================"

# Check prerequisites
if ! curl --version | grep -qi "nghttp2"; then
    echo "WARNING: curl may not have HTTP/2 support (nghttp2 not found in curl --version)"
fi

generate_certs
setup_acme
create_config
start_server
wait_for_server

# Common curl flags
CURL_H2="curl -s --http2 --cacert $TMPDIR/cert.pem"
CURL_H1="curl -s --http1.1 --cacert $TMPDIR/cert.pem"

# ============================================================
# A. ALPN Protocol Negotiation
# ============================================================
section "A. ALPN Protocol Negotiation"

# Test 1: HTTP/2 negotiation via ALPN
RESP=$(curl -s --http2 --cacert "$TMPDIR/cert.pem" -o /dev/null -w "%{http_version}" "${TLS_URL}/health")
if [ "$RESP" = "2" ]; then
    pass "HTTP/2 negotiated via ALPN (http_version=$RESP)"
else
    fail "HTTP/2 not negotiated (http_version=$RESP, expected 2)"
fi

# Test 2: HTTP/1.1 fallback when requested
RESP=$(curl -s --http1.1 --cacert "$TMPDIR/cert.pem" -o /dev/null -w "%{http_version}" "${TLS_URL}/health")
if [ "$RESP" = "1.1" ]; then
    pass "HTTP/1.1 fallback works (http_version=$RESP)"
else
    fail "HTTP/1.1 fallback failed (http_version=$RESP, expected 1.1)"
fi

# Test 3: Verify ALPN protocol string in verbose output
ALPN_INFO=$(curl -v --http2 --cacert "$TMPDIR/cert.pem" -o /dev/null "${TLS_URL}/health" 2>&1)
if echo "$ALPN_INFO" | grep -qi "ALPN.*h2"; then
    pass "ALPN negotiated protocol shows h2"
else
    fail "ALPN h2 string not found in verbose output"
fi

# ============================================================
# B. HTTP/2 Endpoint Parity
# ============================================================
section "B. HTTP/2 Endpoint Parity"

# Test 4: /health over HTTP/2
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/health")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
if [ "$CODE" = "200" ] && echo "$BODY" | grep -q "healthy"; then
    pass "/health returns 200 with healthy status over HTTP/2"
else
    fail "/health over HTTP/2 (got $CODE)"
fi

# Test 5: /health content-type is application/json
CT=$($CURL_H2 -I "${TLS_URL}/health" 2>/dev/null | grep -i "content-type" | tr -d '\r')
if echo "$CT" | grep -qi "application/json"; then
    pass "/health content-type is application/json"
else
    fail "/health content-type (got: $CT)"
fi

# Test 6: /ready over HTTP/2
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/ready")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "/ready returns 200 over HTTP/2"
else
    fail "/ready over HTTP/2 (got $CODE)"
fi

# Test 7: /alive over HTTP/2
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/alive")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "/alive returns 200 over HTTP/2"
else
    fail "/alive over HTTP/2 (got $CODE)"
fi

# Test 8: /metrics over HTTP/2 returns Prometheus format
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/metrics")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
if [ "$CODE" = "200" ] && echo "$BODY" | grep -q "rawrelay_"; then
    pass "/metrics returns Prometheus format over HTTP/2"
else
    fail "/metrics over HTTP/2 (got $CODE)"
fi

# Test 9: /metrics content-type is text/plain
CT=$($CURL_H2 -I "${TLS_URL}/metrics" 2>/dev/null | grep -i "content-type" | tr -d '\r')
if echo "$CT" | grep -qi "text/plain"; then
    pass "/metrics content-type is text/plain"
else
    fail "/metrics content-type (got: $CT)"
fi

# Test 10: / (root) returns 200 home page over HTTP/2
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "200" ]; then
    pass "/ returns 200 home page over HTTP/2"
else
    fail "/ over HTTP/2 (got $CODE, expected 200)"
fi

# Test 11: X-Request-ID header present on HTTP/2 responses
HEADER=$($CURL_H2 -I "${TLS_URL}/health" 2>/dev/null | grep -i "x-request-id" | tr -d '\r')
if echo "$HEADER" | grep -qi "x-request-id"; then
    pass "X-Request-ID header present on HTTP/2 response"
else
    fail "X-Request-ID header missing on HTTP/2 response"
fi

# Test 12: Two HTTP/2 requests get different X-Request-ID values
RID1=$($CURL_H2 -I "${TLS_URL}/health" 2>/dev/null | grep -i "x-request-id" | tr -d '\r' | awk '{print $2}')
RID2=$($CURL_H2 -I "${TLS_URL}/health" 2>/dev/null | grep -i "x-request-id" | tr -d '\r' | awk '{print $2}')
if [ -n "$RID1" ] && [ -n "$RID2" ] && [ "$RID1" != "$RID2" ]; then
    pass "X-Request-ID is unique per stream ($RID1 != $RID2)"
else
    fail "X-Request-ID not unique (rid1='$RID1' rid2='$RID2')"
fi

# Test 13: Cache-Control header present on HTML responses
HEADER=$($CURL_H2 -I "${TLS_URL}/" 2>/dev/null | grep -i "cache-control" | tr -d '\r')
if echo "$HEADER" | grep -qi "cache-control"; then
    pass "Cache-Control header present on HTTP/2 HTML response"
else
    fail "Cache-Control header missing on HTTP/2 HTML response"
fi

# ============================================================
# C. ACME Challenge over HTTP/2
# ============================================================
section "C. ACME Challenge over HTTP/2"

# Test 14: Known ACME token returns 200 with correct content
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/.well-known/acme-challenge/${ACME_TOKEN}")
CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')
if [ "$CODE" = "200" ] && [ "$BODY" = "$ACME_CONTENT" ]; then
    pass "ACME challenge returns 200 with correct body"
else
    fail "ACME challenge (got code=$CODE body='$BODY', expected code=200 body='$ACME_CONTENT')"
fi

# Test 15: Nonexistent token returns 404
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/.well-known/acme-challenge/nonexistent-token-xyz")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "404" ]; then
    pass "ACME nonexistent token returns 404"
else
    fail "ACME nonexistent token (got $CODE, expected 404)"
fi

# Test 16: Path traversal rejected
RESP=$($CURL_H2 -w "\n%{http_code}" --path-as-is "${TLS_URL}/.well-known/acme-challenge/../../../etc/passwd")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "404" ] || [ "$CODE" = "400" ]; then
    pass "ACME path traversal rejected (got $CODE)"
else
    fail "ACME path traversal not rejected (got $CODE, expected 404 or 400)"
fi

# Test 17: Invalid token characters rejected
RESP=$($CURL_H2 -w "\n%{http_code}" "${TLS_URL}/.well-known/acme-challenge/bad<token>chars")
CODE=$(echo "$RESP" | tail -1)
if [ "$CODE" = "404" ] || [ "$CODE" = "400" ]; then
    pass "ACME invalid token chars rejected (got $CODE)"
else
    fail "ACME invalid token chars (got $CODE, expected 404 or 400)"
fi

# ============================================================
# D. TLS Security Hardening
# ============================================================
section "D. TLS Security Hardening"

# Test 18: TLS 1.1 rejected (minimum TLS 1.2 enforced)
TLS11_RESULT=$(echo | openssl s_client -connect "${SERVER_HOST}:${TLS_PORT}" -tls1_1 2>&1)
if echo "$TLS11_RESULT" | grep -qi "error\|alert\|wrong version\|no protocols"; then
    pass "TLS 1.1 rejected (minimum TLS 1.2 enforced)"
else
    fail "TLS 1.1 was accepted (should be rejected)"
fi

# Test 19: TLS 1.2 accepted
TLS12_RESULT=$(echo | openssl s_client -connect "${SERVER_HOST}:${TLS_PORT}" -tls1_2 2>&1)
if echo "$TLS12_RESULT" | grep -q "BEGIN CERTIFICATE\|Verify return code"; then
    pass "TLS 1.2 connection succeeds"
else
    fail "TLS 1.2 connection failed"
fi

# Test 20: Forward secrecy cipher negotiated (ECDHE or DHE)
CIPHER=$(echo | openssl s_client -connect "${SERVER_HOST}:${TLS_PORT}" -tls1_2 2>/dev/null | grep "Cipher is" | awk '{print $NF}')
if echo "$CIPHER" | grep -qi "ECDHE\|DHE"; then
    pass "Forward secrecy cipher negotiated ($CIPHER)"
else
    # TLS 1.3 ciphers don't have ECDHE in name but still use ECDHE key exchange
    TLS13_CIPHER=$(echo | openssl s_client -connect "${SERVER_HOST}:${TLS_PORT}" 2>/dev/null | grep "Cipher is" | awk '{print $NF}')
    if echo "$TLS13_CIPHER" | grep -qi "TLS_AES\|TLS_CHACHA"; then
        pass "TLS 1.3 cipher with forward secrecy ($TLS13_CIPHER)"
    else
        fail "Forward secrecy not enforced (cipher=$CIPHER)"
    fi
fi

# Test 21: Certificate subject matches
CERT_SUBJECT=$(echo | openssl s_client -connect "${SERVER_HOST}:${TLS_PORT}" 2>/dev/null | openssl x509 -noout -subject 2>/dev/null)
if echo "$CERT_SUBJECT" | grep -q "${SERVER_HOST}"; then
    pass "Certificate subject matches server host"
else
    fail "Certificate subject mismatch (got: $CERT_SUBJECT)"
fi

# ============================================================
# E. HTTP/2 Metrics
# ============================================================
section "E. HTTP/2 Metrics"

# Make a few HTTP/2 requests first to populate metrics
$CURL_H2 -o /dev/null "${TLS_URL}/health" 2>/dev/null
$CURL_H2 -o /dev/null "${TLS_URL}/ready" 2>/dev/null

# Give metrics a moment to update
sleep 0.2

# Fetch metrics from HTTP port (simpler, avoids TLS noise in metric counts)
METRICS=$(curl -s "${BASE_URL}/metrics")

# Test 22: h2_streams_total > 0
H2_STREAMS=$(echo "$METRICS" | grep "rawrelay_http2_streams_total" | grep -v "^#" | awk '{print $2}')
if [ -n "$H2_STREAMS" ] && [ "$H2_STREAMS" -gt 0 ] 2>/dev/null; then
    pass "rawrelay_http2_streams_total > 0 ($H2_STREAMS)"
else
    fail "rawrelay_http2_streams_total not > 0 (got: '$H2_STREAMS')"
fi

# Test 23: tls_handshakes_total > 0
TLS_HS=$(echo "$METRICS" | grep "rawrelay_tls_handshakes_total" | grep -v "^#" | head -1 | awk '{print $2}')
if [ -n "$TLS_HS" ] && [ "$TLS_HS" -gt 0 ] 2>/dev/null; then
    pass "rawrelay_tls_handshakes_total > 0 ($TLS_HS)"
else
    fail "rawrelay_tls_handshakes_total not > 0 (got: '$TLS_HS')"
fi

# ============================================================
# F. HTTP/2 Multiplexing
# ============================================================
section "F. HTTP/2 Multiplexing"

# Test 24: Parallel HTTP/2 requests via curl --parallel
PARALLEL_OK=0
PARALLEL_FAIL=0
PARALLEL_RESULT=$(curl -s --http2 --cacert "$TMPDIR/cert.pem" --parallel --parallel-max 5 \
    -w "%{http_code}\n" -o /dev/null \
    -w "%{http_code}\n" -o /dev/null \
    -w "%{http_code}\n" -o /dev/null \
    -w "%{http_code}\n" -o /dev/null \
    -w "%{http_code}\n" -o /dev/null \
    "${TLS_URL}/health" "${TLS_URL}/ready" "${TLS_URL}/alive" "${TLS_URL}/metrics" "${TLS_URL}/health" 2>/dev/null)

for CODE in $PARALLEL_RESULT; do
    if [ "$CODE" = "200" ]; then
        ((PARALLEL_OK++))
    else
        ((PARALLEL_FAIL++))
    fi
done

if [ "$PARALLEL_OK" -eq 5 ]; then
    pass "HTTP/2 multiplexing: 5 parallel requests all returned 200"
else
    fail "HTTP/2 multiplexing: ${PARALLEL_OK}/5 succeeded (${PARALLEL_FAIL} failed)"
fi

# ============================================================
# Summary
# ============================================================

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
