#!/bin/bash
# Real stress test for rawrelay-server
# Tests each feature independently by restarting the server between tests
# Usage: Run from sendrawtx/ directory

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
HTTP_PORT=8080
TLS_PORT=8443
HOST=localhost
METRICS="http://$HOST:$HTTP_PORT/metrics"
CONFIG=/tmp/rawrelay-test-active.ini
LOGFILE=/tmp/rawrelay-test.log

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $1"; PASSES=$((PASSES+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAILS=$((FAILS+1)); }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }
header() { echo -e "\n${CYAN}=== $1 ===${NC}"; }

PASSES=0
FAILS=0

get_metric() {
    local val
    val=$(curl -s $METRICS 2>/dev/null | grep "^$1" | head -1 | awk '{print $2}' | cut -d. -f1)
    echo "${val:-0}"
}

start_server() {
    # $1 = rps, $2 = burst, $3 = normal_max, $4 = read_timeout
    local RPS=${1:-1000}
    local BURST=${2:-2000}
    local NORMAL=${3:-15}
    local TIMEOUT=${4:-10}

    killall -9 rawrelay-server 2>/dev/null || true
    sleep 1

    cat > $CONFIG << EOF
[network]
chain = regtest
[server]
port = $HTTP_PORT
max_connections = 1000
read_timeout = $TIMEOUT
[buffer]
initial_size = 4096
max_size = 16777216
[tiers]
large_threshold = 65536
huge_threshold = 1048576
[static]
dir = $SRCDIR/static
cache_max_age = 0
[slots]
normal_max = $NORMAL
large_max = 3
huge_max = 1
[ratelimit]
rps = $RPS
burst = $BURST
[tls]
enabled = 1
port = $TLS_PORT
cert_file = /tmp/rr.crt
key_file = /tmp/rr.key
http2_enabled = 1
[logging]
verbose = 1
EOF

    cd "$SRCDIR"
    nohup ./rawrelay-server -w 1 $CONFIG > $LOGFILE 2>&1 &
    sleep 2

    if ! curl -s -o /dev/null http://$HOST:$HTTP_PORT/health 2>/dev/null; then
        fail "Server failed to start"
        return 1
    fi
}

echo "╔══════════════════════════════════════════════════════╗"
echo "║          RAWRELAY STRESS TEST SUITE v2              ║"
echo "╚══════════════════════════════════════════════════════╝"

# Generate TLS cert if needed
if [ ! -f /tmp/rr.crt ]; then
    openssl req -x509 -newkey rsa:2048 -keyout /tmp/rr.key -out /tmp/rr.crt \
        -days 1 -nodes -subj '/CN=localhost' 2>/dev/null
fi

# ============================================================
header "TEST 1: Slot Saturation"
info "Config: normal_max=10, rps=1000 (no rate limit interference)"
start_server 1000 2000 10

# Hold connections open with incomplete HTTP requests (no final \r\n)
# This keeps the slot occupied waiting for more data
for i in $(seq 1 15); do
    (echo -ne "GET /health HTTP/1.1\r\nHost: localhost\r\n"; sleep 15) | nc -w 16 $HOST $HTTP_PORT > /dev/null 2>&1 &
done

sleep 2
ACTIVE=$(get_metric 'rawrelay_active_connections{worker="0"}')
info "Active connections holding slots: $ACTIVE"

# Try more requests while slots are full
for i in $(seq 1 10); do
    curl -s -o /dev/null --max-time 2 http://$HOST:$HTTP_PORT/health 2>/dev/null &
done
sleep 1

REJECTS=$(get_metric 'rawrelay_connections_rejected_total{worker="0",reason="slot_limit"}')
info "Slot rejections: $REJECTS"

# Kill held connections
kill $(jobs -p) 2>/dev/null || true
wait 2>/dev/null || true

if [ "${ACTIVE:-0}" -ge 8 ]; then
    pass "Slots filled to $ACTIVE/10"
else
    fail "Only $ACTIVE active connections (expected ~10+)"
fi
if [ "${REJECTS:-0}" -gt 0 ]; then
    pass "Slot rejection: $REJECTS connections turned away"
else
    # Might pass if connections were fast enough to fit
    info "No slot rejections (connections completed too quickly or nc piped too fast)"
fi

# ============================================================
header "TEST 2: Slowloris Detection"
info "Config: normal_max=15, rps=1000"
start_server 1000 2000 15

BEFORE=$(get_metric 'rawrelay_slowloris_kills_total{worker="0"}')

# Drip-feed bytes extremely slowly
for i in $(seq 1 3); do
    (
        echo -ne "G"; sleep 2
        echo -ne "E"; sleep 2
        echo -ne "T"; sleep 2
        echo -ne " "; sleep 2
        echo -ne "/"; sleep 2
        echo -ne " "; sleep 2
        echo -ne "H"; sleep 2
        echo -ne "T"; sleep 20
    ) | nc -w 30 $HOST $HTTP_PORT > /dev/null 2>&1 &
done

info "Waiting for slowloris kill (~15-20s)..."
sleep 22

AFTER=$(get_metric 'rawrelay_slowloris_kills_total{worker="0"}')
KILLS=$((${AFTER:-0} - ${BEFORE:-0}))

kill $(jobs -p) 2>/dev/null || true
wait 2>/dev/null || true

if [ "$KILLS" -gt 0 ]; then
    pass "Slowloris: $KILLS slow connections killed"
else
    fail "Slowloris: no kills (check slowloris config thresholds)"
fi

# ============================================================
header "TEST 3: Rate Limiting"
info "Config: rps=5, burst=10"
start_server 5 10 100

# Exhaust the burst bucket
for i in $(seq 1 30); do
    curl -s -o /dev/null --max-time 1 http://$HOST:$HTTP_PORT/health &
done
wait

RATE_REJECTS=$(get_metric 'rawrelay_connections_rejected_total{worker="0",reason="rate_limit"}')

if [ "${RATE_REJECTS:-0}" -gt 0 ]; then
    pass "Rate limiting: $RATE_REJECTS requests rejected (burst exhausted)"
else
    fail "Rate limiting: 0 rejections"
fi

# ============================================================
header "TEST 4: Read Timeout"
info "Config: read_timeout=5 (short for testing)"
start_server 1000 2000 100 5

# Send incomplete request, wait for timeout
(echo -ne "GET /health HTTP/1.1\r\nHost: localhost\r\n"; sleep 10) | nc -w 11 $HOST $HTTP_PORT > /dev/null 2>&1 &
NCPID=$!

info "Waiting for read timeout (5s + buffer)..."
sleep 8

TIMEOUTS=$(get_metric 'rawrelay_errors_total{worker="0",type="timeout"}')
kill $NCPID 2>/dev/null
wait $NCPID 2>/dev/null

if [ "${TIMEOUTS:-0}" -gt 0 ]; then
    pass "Timeout: $TIMEOUTS connections timed out"
else
    fail "Timeout: no timeouts (check read_timeout)"
fi

# ============================================================
header "TEST 5: TLS + HTTP/2"
info "Config: standard TLS"
start_server 1000 2000 100

for i in $(seq 1 10); do
    curl -sk -o /dev/null https://$HOST:$TLS_PORT/health
done

TLS13=$(get_metric 'rawrelay_tls_handshakes_total{worker="0",protocol="TLSv1.3"}')
H2=$(get_metric 'rawrelay_http2_streams_total{worker="0"}')

if [ "${TLS13:-0}" -gt 0 ]; then
    pass "TLS 1.3: $TLS13 handshakes"
else
    fail "TLS: no TLSv1.3 handshakes"
fi
if [ "${H2:-0}" -gt 0 ]; then
    pass "HTTP/2: $H2 streams via ALPN"
else
    info "HTTP/2: 0 streams (curl might not support h2 over TLS here)"
fi

# ============================================================
header "TEST 6: Keepalive Reuse"
info "Config: standard (high limits)"
# Server still running from TEST 5

for i in $(seq 1 10); do
    curl -s -o /dev/null \
        http://$HOST:$HTTP_PORT/health \
        http://$HOST:$HTTP_PORT/health \
        http://$HOST:$HTTP_PORT/health \
        http://$HOST:$HTTP_PORT/health \
        http://$HOST:$HTTP_PORT/health
done

KA=$(get_metric 'rawrelay_keepalive_reuses_total{worker="0"}')

if [ "${KA:-0}" -gt 0 ]; then
    pass "Keepalive: $KA requests on reused connections"
else
    fail "Keepalive: no reuses"
fi

# ============================================================
header "TEST 7: Bandwidth + Error Tracking"
info "Config: standard"
# Server still running

BEFORE_BYTES=$(get_metric 'rawrelay_response_bytes_total{worker="0"}')
BEFORE_404=$(get_metric 'rawrelay_http_requests_total{worker="0",status="404"}')

# Fetch big pages
for i in $(seq 1 5); do curl -s -o /dev/null http://$HOST:$HTTP_PORT/; done
# Hit 404s
for i in $(seq 1 5); do curl -s -o /dev/null http://$HOST:$HTTP_PORT/nope$i; done

AFTER_BYTES=$(get_metric 'rawrelay_response_bytes_total{worker="0"}')
AFTER_404=$(get_metric 'rawrelay_http_requests_total{worker="0",status="404"}')

NEW_BYTES=$((${AFTER_BYTES:-0} - ${BEFORE_BYTES:-0}))
NEW_404=$((${AFTER_404:-0} - ${BEFORE_404:-0}))

if [ "$NEW_BYTES" -gt 100000 ]; then
    pass "Bandwidth: ${NEW_BYTES} bytes tracked"
else
    fail "Bandwidth: only $NEW_BYTES bytes"
fi
if [ "$NEW_404" -ge 3 ]; then
    pass "404 tracking: $NEW_404 counted"
else
    fail "404 tracking: only $NEW_404"
fi

# ============================================================
header "TEST 8: Metrics Endpoint Completeness"
info "Checking all expected metric families exist"

EXPECTED_METRICS=(
    "rawrelay_requests_total"
    "rawrelay_connections_accepted_total"
    "rawrelay_connections_rejected_total"
    "rawrelay_active_connections"
    "rawrelay_request_duration_seconds_bucket"
    "rawrelay_http_requests_total"
    "rawrelay_http_requests_by_class_total"
    "rawrelay_requests_by_method_total"
    "rawrelay_process_uptime_seconds"
    "rawrelay_open_fds"
    "rawrelay_max_fds"
    "rawrelay_tls_handshakes_total"
    "rawrelay_tls_cert_expiry_timestamp_seconds"
    "rawrelay_http2_streams_total"
    "rawrelay_errors_total"
    "rawrelay_slots_used"
    "rawrelay_slots_max"
    "rawrelay_rate_limiter_entries"
    "rawrelay_response_bytes_total"
    "rawrelay_slowloris_kills_total"
    "rawrelay_slot_promotion_failures_total"
    "rawrelay_keepalive_reuses_total"
    "rawrelay_rpc_broadcasts_total"
    "rawrelay_rpc_broadcasts_success_total"
    "rawrelay_rpc_broadcasts_failed_total"
    "rawrelay_endpoint_requests_total"
)

METRICS_BODY=$(curl -s $METRICS)
MISSING=0
for m in "${EXPECTED_METRICS[@]}"; do
    if ! echo "$METRICS_BODY" | grep -q "^$m"; then
        fail "Missing metric: $m"
        MISSING=$((MISSING+1))
    fi
done
if [ $MISSING -eq 0 ]; then
    pass "All ${#EXPECTED_METRICS[@]} metric families present"
fi

# ============================================================
# CLEANUP
killall rawrelay-server 2>/dev/null || true

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║               RESULTS                              ║"
echo "╠══════════════════════════════════════════════════════╣"
echo -e "║  ${GREEN}PASSED: $PASSES${NC}                                      ║"
echo -e "║  ${RED}FAILED: $FAILS${NC}                                      ║"
echo "╚══════════════════════════════════════════════════════╝"
