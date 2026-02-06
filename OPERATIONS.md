# Operations Guide

This covers everything you need to run sendrawtx in production. For building and contributing, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Signals

| Signal | Recipient | Action |
|--------|-----------|--------|
| SIGTERM | Master | Graceful shutdown — drains all workers, waits for connections to finish |
| SIGINT | Master | Same as SIGTERM |
| SIGHUP | Master | Graceful reload — forks new workers with updated config, drains old ones (30s timeout, then SIGKILL) |
| SIGUSR1 | Worker | Graceful drain — stops accepting connections, exits when all active connections close |
| SIGUSR2 | Worker | TLS certificate reload — reloads cert and key from disk without restart |
| SIGPIPE | All | Ignored |

SIGUSR2 is useful after `certbot renew` — reload the new cert without dropping connections:

```bash
kill -USR2 $(pgrep rawrelay-server)
```

New connections get the new cert. Existing TLS connections continue with the old one until they close naturally.

## Health Endpoint

`GET /health` returns JSON with detailed worker status:

```json
{
  "status": "healthy",
  "worker_id": 0,
  "uptime_seconds": 3600,
  "active_connections": 12,
  "requests_processed": 84520,
  "slots": {
    "normal": { "used": 10, "max": 100 },
    "large": { "used": 2, "max": 20 },
    "huge": { "used": 0, "max": 5 }
  },
  "rate_limiter_entries": 47,
  "tls": {
    "enabled": true,
    "cert_expires_in_days": 62,
    "cert_expiry_warning": false
  },
  "resources": {
    "open_fds": 34,
    "max_fds": 1024,
    "fd_usage_percent": 3.32
  }
}
```

`cert_expiry_warning` goes true when the cert has less than 30 days left. `fd_usage_percent` is only available on Linux (reads `/proc/self/fd`).

Each worker serves its own `/health` — if you have 4 workers, you'll get different `worker_id` values depending on which one handles your request.

## Metrics Endpoint

`GET /metrics` returns Prometheus text format. Useful for scraping with Prometheus, Grafana, etc.

Key metrics:

**Traffic:**
- `rawrelay_requests_total{worker="N"}` — total requests handled
- `rawrelay_connections_accepted_total{worker="N"}` — total connections accepted
- `rawrelay_active_connections{worker="N"}` — current open connections

**Rejections:**
- `rawrelay_connections_rejected_total{worker="N",reason="rate_limit"}` — rejected by rate limiter
- `rawrelay_connections_rejected_total{worker="N",reason="slot_limit"}` — rejected by slot exhaustion
- `rawrelay_connections_rejected_total{worker="N",reason="blocked"}` — rejected by IP blocklist

**HTTP status codes:**
- `rawrelay_http_requests_total{worker="N",status="200|400|404|408|429|503"}`
- `rawrelay_http_requests_by_class_total{worker="N",class="2xx|4xx|5xx"}`

**Latency histogram (cumulative buckets):**
- `rawrelay_request_duration_seconds_bucket{worker="N",le="0.001|0.005|0.01|0.05|0.1|0.5|1|5|+Inf"}`

**TLS:**
- `rawrelay_tls_handshakes_total{worker="N",protocol="TLSv1.2|TLSv1.3"}`
- `rawrelay_tls_cert_expiry_timestamp_seconds{worker="N"}` — unix timestamp of cert expiry

**HTTP/2:**
- `rawrelay_http2_streams_total{worker="N"}` — total h2 streams opened
- `rawrelay_http2_streams_active{worker="N"}` — current active streams

**Slots:**
- `rawrelay_slots_used{worker="N",tier="normal|large|huge"}`
- `rawrelay_slots_max{worker="N",tier="normal|large|huge"}`

All metrics are per-worker. Prometheus should aggregate across workers for system-wide totals.

## Rate Limiting

Token bucket algorithm, per-IP, per-worker.

```ini
[ratelimit]
rps = 100    # tokens added per second
burst = 200  # max tokens in the bucket
```

Each request costs 1 token. Tokens refill at `rps` per second up to `burst`. When the bucket is empty, the client gets 429 Too Many Requests.

**Per-worker math:** Each worker tracks rate limits independently. With 4 workers and `rps = 100`, a single IP could theoretically send 400 requests/second across all workers. Set `rps` accordingly.

**Allowlist bypass:** IPs in the allowlist skip rate limiting entirely.

**Limits:** Each worker tracks up to 10,000 unique IPs. Entries expire after 60 seconds of inactivity. If the table fills up, new IPs are denied (fail-safe).

**Disabling:** Set `rps = 0` to disable rate limiting.

## IP Access Control

Blocklist and allowlist files, one entry per line.

```ini
[security]
blocklist_file = /etc/rawrelay/blocklist.txt
allowlist_file = /etc/rawrelay/allowlist.txt
```

**File format:**

```
# Comments start with #
# Empty lines ignored

# Exact IPs
192.168.1.1
2001:db8::1

# CIDR ranges
10.0.0.0/8
192.168.0.0/16
2001:db8::/32
```

**Precedence:** Blocklist is checked first. If an IP matches both lists, it's blocked.

- **Blocked** → 403 Forbidden
- **Allowlisted** → bypasses rate limiting
- **Neither** → normal rate limiting applies

**Hot-reload:** Send SIGHUP to the master process. New workers start with the updated files. Old workers drain and exit.

## Connection Slots

Three tiers prevent large requests from starving small ones.

```ini
[slots]
normal_max = 100   # default tier
large_max = 20     # requests > 64KB
huge_max = 5       # requests > 1MB

[tiers]
large_threshold = 65536     # 64KB - promotes to large tier
huge_threshold = 1048576    # 1MB - promotes to huge tier
```

**How it works:**

1. New connection gets a `normal` slot
2. As data arrives, if the request exceeds `large_threshold`, the connection promotes to a `large` slot (frees the normal slot)
3. If it exceeds `huge_threshold`, promotes again to `huge`
4. After the request body is fully received, the connection downgrades back to `normal` for the response phase
5. On keep-alive, the slot resets to `normal`

If no slots are available at any tier, the client gets 503 Service Unavailable.

**Total system capacity:**

```
Total connections = num_workers × (normal_max + large_max + huge_max)
Example: 4 workers × 125 slots = 500 concurrent connections
```

These are per-worker. No shared state, no locks.

## Slowloris Protection

The server detects slow-sending clients and kills their connections.

Three checks run during request reading:

| Check | Threshold | Action |
|-------|-----------|--------|
| Total time | > 120 seconds | Kill connection |
| Throughput | < 100 bytes per 5-second window | Kill connection |
| Buffer regression | Buffer shrinks between checks | Kill connection |

Legitimate clients that send data at any reasonable speed are unaffected. These thresholds catch attackers that trickle data one byte at a time to hold connections open.

The timer resets on keep-alive, so long-lived connections with normal request/response cycles are fine.

## RPC Configuration

Connect to a Bitcoin Core node for transaction broadcasting.

```ini
[network]
chain = mainnet    # mainnet, testnet, signet, or regtest
```

**Authentication** — use one of:

Cookie auth (preferred, automatic with Bitcoin Core):
```ini
[rpc]
cookie_file = /home/bitcoin/.bitcoin/.cookie
```

Or datadir auto-discovery:
```ini
[rpc]
datadir = /home/bitcoin/.bitcoin
```

Or username/password:
```ini
[rpc]
user = bitcoinrpc
password = yourpassword
```

**Connection settings:**
```ini
[rpc]
host = 127.0.0.1
port = 8332         # 8332=mainnet, 18332=testnet, 38332=signet, 18443=regtest
timeout = 30        # seconds
wallet = ""         # optional wallet name
```

The server supports all four networks simultaneously. Each chain has its own RPC client with independent connection tracking.

**Error handling:** If the RPC connection fails, the server logs the error and returns an error response to the client. It does not retry automatically — the next request will attempt a fresh connection.

## TLS Setup

```ini
[tls]
enabled = 1
port = 8443
cert_file = /etc/letsencrypt/live/sendrawtx.com/fullchain.pem
key_file = /etc/letsencrypt/live/sendrawtx.com/privkey.pem
http2_enabled = 1
```

**Minimum version:** TLS 1.2 (no TLS 1.0 or 1.1).

**ALPN:** When `http2_enabled = 1`, the server negotiates HTTP/2 (`h2`) or falls back to HTTP/1.1 based on client support.

**Certificate reload:** Send SIGUSR2 to reload certs from disk without restarting. See [Signals](#signals).

**Self-signed certs for testing:**

```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"
```

## Seccomp

Optional Linux syscall filter that restricts worker processes to a whitelist of allowed syscalls. Any disallowed syscall kills the worker immediately.

```ini
[security]
seccomp = 1
```

**Off by default** because:
- Linux-only (no-op on macOS)
- Architecture-dependent (x86-64, ARM64, ARM32, i386)
- Third-party libraries (OpenSSL, libevent, nghttp2) may use syscalls not in the whitelist
- A violation kills the worker with no log output — hard to debug

**When to enable:** After you've tested your exact deployment (OS, architecture, library versions) and confirmed the server works normally under load with seccomp on.

The whitelist covers: network I/O, memory management, file operations, epoll/poll, timers, signals, and process info. It blocks: fork, exec, ptrace, mount, and anything else not explicitly listed.
