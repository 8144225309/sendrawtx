# sendrawtx

[![CI](https://github.com/8144225309/sendrawtx/actions/workflows/ci.yml/badge.svg)](https://github.com/8144225309/sendrawtx/actions/workflows/ci.yml)

High-performance Bitcoin raw transaction relay server, written in C. Uses a local Bitcoin Core node to serve data over HTTP.

## Features

- Multi-process architecture with SO_REUSEPORT (kernel-level load balancing)
- HTTP/1.1 and HTTP/2 with TLS/ALPN negotiation
- Per-IP rate limiting (token bucket)
- IP blocklist/allowlist with CIDR support
- Tiered connection slots to prevent resource exhaustion
- Slowloris attack detection and mitigation
- Optional seccomp syscall filtering (Linux)
- Graceful reload and shutdown via signals
- Zero-copy buffer handling with libevent
- Prometheus-compatible metrics endpoint

## Quick Start

### 1. Install dependencies

```bash
# Ubuntu/Debian
sudo apt install build-essential libevent-dev libssl-dev libnghttp2-dev pkg-config

# macOS
brew install libevent openssl nghttp2 pkg-config
```

### 2. Build

```bash
make clean && make
```

Compiles with `-Wall -Wextra -Werror`. No warnings allowed.

### 3. Configure

```bash
cp config.ini.example config.ini
```

See `config.ini.example` for all options with inline documentation.

### 4. Run

```bash
./rawrelay-server                  # auto-detect workers (1 per CPU)
./rawrelay-server -w 4             # 4 workers
./rawrelay-server -t               # test config and exit
./rawrelay-server /path/to/config  # custom config file
```

The server listens on port 8080 (HTTP) and optionally 8443 (HTTPS/HTTP2) if TLS is enabled.

## Bitcoin Transaction Relay

The current application relays raw Bitcoin transactions through a Bitcoin Core node via JSON-RPC.

```
User                         sendrawtx                    Bitcoin Core
 │                              │                              │
 │  GET /{raw_tx_hex}           │                              │
 │─────────────────────────────>│  sendrawtransaction(hex)     │
 │                              │─────────────────────────────>│
 │  broadcast.html              │                              │
 │<─────────────────────────────│          result              │
 │                              │<─────────────────────────────│
 │  (client computes txid,      │                              │
 │   redirects to /tx/{txid})   │                              │
 │                              │                              │
 │  GET /tx/{txid}              │                              │
 │─────────────────────────────>│                              │
 │  result.html (status page)   │                              │
 │<─────────────────────────────│                              │
```

### Transaction Endpoints

| Path | Description |
|------|-------------|
| `/{raw-tx-hex}` | Broadcast a raw transaction. Serves a page that computes the txid and redirects to the status page. |
| `/tx/{txid}` | Transaction status. Shows broadcast result, confirmations, and a link to mempool.space. |
| `/{txid}` | Shortcut for `/tx/{txid}` (exactly 64 hex characters). |

### RPC Configuration

Point the server at your Bitcoin Core node:

```ini
[network]
chain = mainnet              # mainnet, testnet, signet, or regtest

[rpc]
host = 127.0.0.1
port = 8332                  # 8332=mainnet, 18332=testnet, 38332=signet, 18443=regtest
cookie_file = /home/bitcoin/.bitcoin/.cookie
```

Supports cookie auth (preferred), datadir auto-discovery, or username/password. See [OPERATIONS.md](OPERATIONS.md) for full RPC setup.

## Server Endpoints

| Path | Description |
|------|-------------|
| `/health` | JSON with worker status, connection counts, slot usage, TLS cert expiry. |
| `/ready` | 200 if accepting traffic, 503 if draining. Use as a load balancer health check. |
| `/alive` | Always returns 200. Liveness probe. |
| `/metrics` | Prometheus-format metrics (request counts, latency histograms, error rates, slot usage). |

## Architecture

```
┌────────────────────────────────────────────┐
│              Master Process                │
│  Forks workers, monitors health,           │
│  handles reload (SIGHUP) and shutdown      │
└──────────────────┬─────────────────────────┘
                   │
     ┌─────────────┼─────────────┐
     ▼             ▼             ▼
┌──────────┐ ┌──────────┐ ┌──────────┐
│ Worker 0 │ │ Worker 1 │ │ Worker N │
│ Core 0   │ │ Core 1   │ │ Core N   │
│ Own sock │ │ Own sock │ │ Own sock │
│ Own loop │ │ Own loop │ │ Own loop │
│ No locks │ │ No locks │ │ No locks │
└──────────┘ └──────────┘ └──────────┘
     │             │             │
     └─────────────┼─────────────┘
                   ▼
          ┌────────────────┐
          │  Bitcoin Core  │
          │  (JSON-RPC)    │
          └────────────────┘
```

Each worker independently accepts connections via `SO_REUSEPORT`, handles HTTP parsing, serves responses, and communicates with the backend. No shared state between workers.

## Security

- Per-IP rate limiting with configurable burst
- IP blocklist/allowlist with CIDR and hot-reload
- Tiered connection slots (normal/large/huge)
- Slowloris detection (throughput monitoring)
- Seccomp syscall filtering (optional, Linux)
- TLS 1.2+ only, no compression (CRIME mitigation)

Report vulnerabilities via [GitHub Security Advisories](https://github.com/8144225309/sendrawtx/security/advisories/new). See [SECURITY.md](SECURITY.md).

## Testing

```bash
make test                        # unit tests
./tests/test_integration.sh      # end-to-end HTTP tests
```

See [TESTING.md](TESTING.md) for the full testing strategy (sanitizers, fuzzing, stress tests).

## Documentation

| Document | Contents |
|----------|----------|
| [OPERATIONS.md](OPERATIONS.md) | Rate limiting, IP ACLs, connection slots, TLS, signals, metrics, RPC, seccomp |
| [TESTING.md](TESTING.md) | Testing strategy, CI pipeline, how to add tests |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Build instructions, code style, PR process |
| [SECURITY.md](SECURITY.md) | Vulnerability reporting policy |
| `config.ini.example` | All configuration options with inline docs |

## License

MIT
