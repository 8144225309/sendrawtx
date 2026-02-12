#!/bin/bash
# Sustained stress test for Grafana visualization
# Uses apache bench (ab) for real sustained load
# Each wave lasts 20-30s so Prometheus scrapes capture everything
HOST=localhost
HTTP=8080
TLS=8443
CONC=50  # concurrency for ab

echo "============================================"
echo "  RAWRELAY SUSTAINED ATTACK"
echo "  Grafana: http://localhost:3000"
echo "============================================"
echo ""

metric() {
    curl -s http://$HOST:$HTTP/metrics 2>/dev/null | grep "^$1" | head -1 | awk '{print $2}' | cut -d. -f1
}

# ── WAVE 1: Sustained HTTP Flood (30s) ─────────
echo "[WAVE 1/8] SUSTAINED HTTP FLOOD"
echo "  ab: 10000 requests, 50 concurrent, ~30s sustained"
echo "  >> Requests/sec, Bandwidth, Active Connections"
ab -n 10000 -c $CONC -s 2 -q http://$HOST:$HTTP/health 2>&1 | grep -E "Requests per|Complete|Failed|Transfer rate"
echo ""
sleep 5

# ── WAVE 2: Slot Saturation + Overflow (30s) ───
echo "[WAVE 2/8] SLOT SATURATION"
echo "  20 nc connections hold slots, then ab flood hits slot_limit wall"
echo "  >> Slots Used fills to 15/15, Slot Rejections spike"
# Hold 20 incomplete HTTP connections (only 15 slots available)
for i in $(seq 1 20); do
    (printf "GET /health HTTP/1.1\r\nHost: localhost\r\n"; sleep 30) | nc -w 31 $HOST $HTTP > /dev/null 2>&1 &
done
sleep 3
SLOTS=$(metric 'rawrelay_slots_used{worker="0",tier="normal"}')
echo "  Slots filled: ${SLOTS:-?}/15"
# Now hammer while slots are full
ab -n 2000 -c 20 -s 2 -q http://$HOST:$HTTP/health 2>&1 | grep -E "Requests per|Complete|Failed|Non-2xx"
REJECTS=$(metric 'rawrelay_connections_rejected_total{worker="0",reason="slot_limit"}')
echo "  Slot rejections so far: ${REJECTS:-0}"
# Let the nc connections time out naturally
sleep 10
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
echo ""
sleep 5

# ── WAVE 3: Rate Limit Exhaustion (30s) ────────
echo "[WAVE 3/8] RATE LIMIT STORM"
echo "  ab: 5000 requests, 100 concurrent (rps=50, burst=100 => most rejected)"
echo "  >> Rate Limit Rejections counter rockets"
ab -n 5000 -c 100 -s 1 -q http://$HOST:$HTTP/health 2>&1 | grep -E "Requests per|Complete|Failed|Non-2xx"
RATE_R=$(metric 'rawrelay_connections_rejected_total{worker="0",reason="rate_limit"}')
echo "  Rate limit rejections so far: ${RATE_R:-0}"
echo ""
sleep 8

# ── WAVE 4: Slowloris Siege (40s) ──────────────
echo "[WAVE 4/8] SLOWLORIS SIEGE"
echo "  10 connections drip-feeding 1 byte every 3 seconds"
echo "  >> Slowloris Kills counter, Timeout errors"
BEFORE=$(metric 'rawrelay_slowloris_kills_total{worker="0"}')
for i in $(seq 1 10); do
    (
        printf "G"; sleep 3
        printf "E"; sleep 3
        printf "T"; sleep 3
        printf " "; sleep 3
        printf "/"; sleep 3
        printf " "; sleep 25
    ) | nc -w 30 $HOST $HTTP > /dev/null 2>&1 &
done
echo "  10 slowloris connections dripping..."
# Meanwhile send normal traffic so dashboard stays active
for round in $(seq 1 8); do
    ab -n 100 -c 5 -s 2 -q http://$HOST:$HTTP/health > /dev/null 2>&1
    sleep 3
done
AFTER=$(metric 'rawrelay_slowloris_kills_total{worker="0"}')
KILLS=$(( ${AFTER:-0} - ${BEFORE:-0} ))
echo "  Slowloris kills this wave: $KILLS"
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
echo ""
sleep 5

# ── WAVE 5: TLS + HTTP/2 Storm (30s) ──────────
echo "[WAVE 5/8] TLS + HTTP/2 STORM"
echo "  50 concurrent TLS connections, sustained"
echo "  >> TLS Handshakes, HTTP/2 Streams"
# ab doesn't do TLS easily, use curl loops sustained over 20s
END=$((SECONDS + 20))
while [ $SECONDS -lt $END ]; do
    for i in $(seq 1 10); do
        curl -sk -o /dev/null --max-time 3 https://$HOST:$TLS/health &
    done
    sleep 1
done
wait
TLS13=$(metric 'rawrelay_tls_handshakes_total{worker="0",protocol="TLSv1.3"}')
H2=$(metric 'rawrelay_http2_streams_total{worker="0"}')
echo "  TLS 1.3 handshakes: ${TLS13:-0}"
echo "  HTTP/2 streams: ${H2:-0}"
echo ""
sleep 5

# ── WAVE 6: Keepalive Saturation (20s) ────────
echo "[WAVE 6/8] KEEPALIVE SATURATION"
echo "  ab with keepalive (-k), 5000 requests on reused connections"
echo "  >> Keepalive Reuses counter jumps massively"
BEFORE_KA=$(metric 'rawrelay_keepalive_reuses_total{worker="0"}')
ab -n 5000 -c 20 -k -s 2 -q http://$HOST:$HTTP/health 2>&1 | grep -E "Requests per|Complete|Keep-Alive"
AFTER_KA=$(metric 'rawrelay_keepalive_reuses_total{worker="0"}')
NEW_KA=$(( ${AFTER_KA:-0} - ${BEFORE_KA:-0} ))
echo "  New keepalive reuses: $NEW_KA"
echo ""
sleep 5

# ── WAVE 7: 404 Error Flood + Bandwidth (20s) ─
echo "[WAVE 7/8] ERROR + BANDWIDTH FLOOD"
echo "  Mix of valid pages (bandwidth) and 404s (error counters)"
echo "  >> 4xx class spikes, Response Bytes climbs"
# Fetch the homepage (big HTML) repeatedly for bandwidth
ab -n 2000 -c 20 -s 2 -q http://$HOST:$HTTP/ > /dev/null 2>&1 &
PID_BW=$!
# Simultaneously hit 404s
ab -n 2000 -c 20 -s 2 -q http://$HOST:$HTTP/nonexistent_page > /dev/null 2>&1 &
PID_ERR=$!
wait $PID_BW $PID_ERR
BYTES=$(metric 'rawrelay_response_bytes_total{worker="0"}')
FOUR=$(metric 'rawrelay_http_requests_by_class_total{worker="0",class="4xx"}')
echo "  Total bytes served: ${BYTES:-0}"
echo "  Total 4xx responses: ${FOUR:-0}"
echo ""
sleep 5

# ── WAVE 8: TOTAL CHAOS (45s) ─────────────────
echo "[WAVE 8/8] TOTAL CHAOS - everything simultaneously for 45 seconds"
echo "  >> ALL charts spike at once"

# Sustained HTTP flood
ab -n 10000 -c 30 -s 2 -q http://$HOST:$HTTP/health > /dev/null 2>&1 &
PID1=$!

# Sustained keepalive flood
ab -n 5000 -c 20 -k -s 2 -q http://$HOST:$HTTP/ > /dev/null 2>&1 &
PID2=$!

# Sustained 404s
ab -n 3000 -c 10 -s 2 -q http://$HOST:$HTTP/chaos_404 > /dev/null 2>&1 &
PID3=$!

# Slot holders
for i in $(seq 1 20); do
    (printf "GET /health HTTP/1.1\r\nHost: localhost\r\n"; sleep 40) | nc -w 41 $HOST $HTTP > /dev/null 2>&1 &
done

# TLS storm
END=$((SECONDS + 40))
while [ $SECONDS -lt $END ]; do
    for i in $(seq 1 5); do
        curl -sk -o /dev/null --max-time 2 https://$HOST:$TLS/health &
    done
    sleep 2
done &
PID4=$!

# Slowloris sprinkled in
for i in $(seq 1 5); do
    (printf "P"; sleep 3; printf "O"; sleep 3; printf "S"; sleep 30) | nc -w 35 $HOST $HTTP > /dev/null 2>&1 &
done

echo "  Chaos running for ~45 seconds..."
sleep 45
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
echo "  Chaos complete"

echo ""
echo "============================================"
echo "  ATTACK COMPLETE - FINAL METRICS"
echo "============================================"
echo ""
echo "--- Worker 0 ---"
curl -s http://$HOST:$HTTP/metrics 2>/dev/null | grep -E '^rawrelay_(requests_total|connections_accepted|connections_rejected|keepalive|slowloris|tls_handshakes|http2_streams_total|slots_used|http_requests_by_class|response_bytes|errors_total)\{worker="0"' | sort
echo ""
echo "--- Worker 1 ---"
curl -s http://$HOST:$HTTP/metrics 2>/dev/null | grep -E '^rawrelay_(requests_total|connections_accepted|connections_rejected|keepalive|slowloris|tls_handshakes|http2_streams_total|slots_used|http_requests_by_class|response_bytes|errors_total)\{worker="1"' | sort
