#!/bin/bash
# Industry-standard hardening test suite
# Tools: slowhttptest, h2load, wrk, raw sockets
HOST=localhost
HTTP=8080
TLS=8443

metric() {
    curl -s http://$HOST:$HTTP/metrics 2>/dev/null | grep "^$1" | head -1 | awk '{print $2}' | cut -d. -f1
}

echo "============================================"
echo "  RAWRELAY HARDENING SUITE"
echo "  Tools: slowhttptest, h2load, wrk, fuzz"
echo "============================================"

# ── TEST 1: wrk sustained load with latency stats ──
echo ""
echo "[1/9] WRK - Sustained HTTP load (30s)"
echo "  200 connections, 4 threads, latency percentiles"
wrk -t4 -c200 -d30s --latency http://$HOST:$HTTP/health
sleep 5

# ── TEST 2: wrk keepalive pipeline ─────────────────
echo ""
echo "[2/9] WRK - Keepalive pipeline (20s)"
echo "  50 persistent connections hammering /"
BEFORE_KA=$(metric 'rawrelay_keepalive_reuses_total{worker="0"}')
wrk -t2 -c50 -d20s http://$HOST:$HTTP/
AFTER_KA=$(metric 'rawrelay_keepalive_reuses_total{worker="0"}')
echo "  Keepalive reuses this wave: $(( ${AFTER_KA:-0} - ${BEFORE_KA:-0} ))"
sleep 5

# ── TEST 3: slowhttptest - Slowloris mode ──────────
echo ""
echo "[3/9] SLOWHTTPTEST - Slowloris mode (30s)"
echo "  200 connections, send 1 byte every 10 seconds"
echo "  Server should kill these before they exhaust slots"
BEFORE_SL=$(metric 'rawrelay_slowloris_kills_total{worker="0"}')
slowhttptest -c 200 -H -g -o /tmp/slowloris_results \
    -i 10 -r 50 -t GET -u http://$HOST:$HTTP/health \
    -l 30 -p 3 2>&1 | tail -5
AFTER_SL=$(metric 'rawrelay_slowloris_kills_total{worker="0"}')
echo "  Slowloris kills: $(( ${AFTER_SL:-0} - ${BEFORE_SL:-0} ))"
sleep 5

# ── TEST 4: slowhttptest - Slow POST body ─────────
echo ""
echo "[4/9] SLOWHTTPTEST - Slow POST body (30s)"
echo "  Sends Content-Length then drips body 1 byte/10s"
BEFORE_TO=$(metric 'rawrelay_errors_total{worker="0",type="timeout"}')
slowhttptest -c 200 -B -g -o /tmp/slowpost_results \
    -i 10 -r 50 -t POST -u http://$HOST:$HTTP/api/tx \
    -l 30 -p 3 2>&1 | tail -5
AFTER_TO=$(metric 'rawrelay_errors_total{worker="0",type="timeout"}')
echo "  Timeouts: $(( ${AFTER_TO:-0} - ${BEFORE_TO:-0} ))"
sleep 5

# ── TEST 5: slowhttptest - Slow Read ──────────────
echo ""
echo "[5/9] SLOWHTTPTEST - Slow Read attack (30s)"
echo "  Reads response as slowly as possible"
slowhttptest -c 200 -X -g -o /tmp/slowread_results \
    -r 50 -t GET -u http://$HOST:$HTTP/ \
    -l 30 -p 3 -w 1 -y 1 -n 1 -z 1 2>&1 | tail -5
sleep 5

# ── TEST 6: h2load - HTTP/2 stream flood ──────────
echo ""
echo "[6/9] H2LOAD - HTTP/2 stream flood (20s)"
echo "  10 connections x 100 concurrent streams = 1000 parallel"
h2load -n 10000 -c 10 -m 100 -t 2 \
    https://$HOST:$TLS/health 2>&1 | grep -E "finished|requests:|status codes:|traffic:"
H2=$(metric 'rawrelay_http2_streams_total{worker="0"}')
echo "  HTTP/2 streams total: ${H2:-0}"
sleep 5

# ── TEST 7: Fuzz - binary garbage ─────────────────
echo ""
echo "[7/9] FUZZ - Binary garbage, oversized headers, malformed HTTP"
BEFORE_PE=$(metric 'rawrelay_errors_total{worker="0",type="parse_error"}')

echo "  a) 20 connections of /dev/urandom (should not crash)"
for i in $(seq 1 20); do
    (head -c 4096 /dev/urandom | nc -w 3 $HOST $HTTP > /dev/null 2>&1) &
done
wait
echo "     Server alive: $(curl -s -o /dev/null -w '%{http_code}' http://$HOST:$HTTP/health)"

echo "  b) 10 oversized header attacks (100KB headers)"
for i in $(seq 1 10); do
    (
        printf "GET / HTTP/1.1\r\nHost: localhost\r\n"
        printf "X-Garbage: "
        head -c 102400 /dev/urandom | base64 | tr -d '\n'
        printf "\r\n\r\n"
    ) | nc -w 3 $HOST $HTTP > /dev/null 2>&1 &
done
wait
echo "     Server alive: $(curl -s -o /dev/null -w '%{http_code}' http://$HOST:$HTTP/health)"

echo "  c) 10 malformed HTTP lines"
for i in $(seq 1 10); do
    (
        printf "GARBAGEMETHOD /not/real HXXP/9.9\r\n\r\n"
    ) | nc -w 3 $HOST $HTTP > /dev/null 2>&1 &
done
wait
echo "     Server alive: $(curl -s -o /dev/null -w '%{http_code}' http://$HOST:$HTTP/health)"

echo "  d) 10 incomplete requests (just newlines)"
for i in $(seq 1 10); do
    (printf "\r\n\r\n\r\n") | nc -w 3 $HOST $HTTP > /dev/null 2>&1 &
done
wait
echo "     Server alive: $(curl -s -o /dev/null -w '%{http_code}' http://$HOST:$HTTP/health)"

AFTER_PE=$(metric 'rawrelay_errors_total{worker="0",type="parse_error"}')
echo "  Parse errors: $(( ${AFTER_PE:-0} - ${BEFORE_PE:-0} ))"
sleep 5

# ── TEST 8: Connection exhaustion ─────────────────
echo ""
echo "[8/9] CONNECTION EXHAUSTION - 500 raw TCP sockets held open"
echo "  Open sockets, hold them, see if server still serves new ones"
for i in $(seq 1 500); do
    (sleep 20 | nc -w 21 $HOST $HTTP > /dev/null 2>&1) &
done
sleep 3
ACTIVE=$(metric 'rawrelay_active_connections{worker="0"}')
SLOTS=$(metric 'rawrelay_slots_used{worker="0",tier="normal"}')
echo "  Active connections: ${ACTIVE:-0}"
echo "  Slots used: ${SLOTS:-0}/15"

echo "  Can we still get through?"
RESP=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://$HOST:$HTTP/health)
echo "  Health check response: $RESP"

SLOT_REJ=$(metric 'rawrelay_connections_rejected_total{worker="0",reason="slot_limit"}')
RATE_REJ=$(metric 'rawrelay_connections_rejected_total{worker="0",reason="rate_limit"}')
echo "  Slot rejections total: ${SLOT_REJ:-0}"
echo "  Rate rejections total: ${RATE_REJ:-0}"

kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
sleep 5

# ── TEST 9: EVERYTHING AT ONCE (60s) ─────────────
echo ""
echo "[9/9] ARMAGEDDON - all tools simultaneously for 60 seconds"
echo "  wrk + h2load + slowhttptest + fuzz + connection exhaustion"

# wrk sustained flood
wrk -t2 -c100 -d60s http://$HOST:$HTTP/health > /tmp/arma_wrk.txt 2>&1 &
PID_WRK=$!

# h2load stream flood
h2load -n 50000 -c 5 -m 50 -t 2 https://$HOST:$TLS/health > /tmp/arma_h2.txt 2>&1 &
PID_H2=$!

# slowhttptest slowloris
slowhttptest -c 100 -H -i 10 -r 25 -t GET \
    -u http://$HOST:$HTTP/health -l 55 -p 3 > /tmp/arma_slow.txt 2>&1 &
PID_SLOW=$!

# Connection holders
for i in $(seq 1 50); do
    (printf "GET / HTTP/1.1\r\nHost: localhost\r\n"; sleep 55) | nc -w 56 $HOST $HTTP > /dev/null 2>&1 &
done

# Periodic fuzz bursts
for round in $(seq 1 6); do
    for i in $(seq 1 5); do
        (head -c 2048 /dev/urandom | nc -w 2 $HOST $HTTP > /dev/null 2>&1) &
    done
    sleep 10
done &
PID_FUZZ=$!

# TLS + 404 mix
for round in $(seq 1 12); do
    for i in $(seq 1 5); do
        curl -sk -o /dev/null --max-time 2 https://$HOST:$TLS/health &
        curl -s -o /dev/null --max-time 2 http://$HOST:$HTTP/nope_$round &
    done
    sleep 5
done &
PID_MIX=$!

echo "  All weapons firing..."
wait $PID_WRK
echo ""
echo "  -- wrk results --"
cat /tmp/arma_wrk.txt | grep -E "Latency|Req/Sec|requests in|Transfer|Socket errors|Non-2xx"
echo ""
echo "  -- h2load results --"
wait $PID_H2 2>/dev/null
cat /tmp/arma_h2.txt 2>/dev/null | grep -E "finished|requests:|status codes:|traffic:"

kill $(jobs -p) 2>/dev/null
wait 2>/dev/null

echo ""
echo "============================================"
echo "  HARDENING SUITE COMPLETE - FINAL REPORT"
echo "============================================"
echo ""

# Check server survived
ALIVE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://$HOST:$HTTP/health)
echo "SERVER STATUS: $ALIVE"
echo ""

echo "--- Worker 0 ---"
curl -s http://$HOST:$HTTP/metrics 2>/dev/null | grep -E '^rawrelay_(requests_total|connections_accepted|connections_rejected|keepalive|slowloris|tls_handshakes|http2_streams_total|http_requests_by_class|response_bytes|errors_total|slots_used|rate_limiter)\{worker="0"' | sort
echo ""
echo "--- Worker 1 ---"
curl -s http://$HOST:$HTTP/metrics 2>/dev/null | grep -E '^rawrelay_(requests_total|connections_accepted|connections_rejected|keepalive|slowloris|tls_handshakes|http2_streams_total|http_requests_by_class|response_bytes|errors_total|slots_used|rate_limiter)\{worker="1"' | sort
