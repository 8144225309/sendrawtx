# RawRelay Server

High-performance Bitcoin raw transaction relay server.

## Features

- Multi-process architecture with SO_REUSEPORT (kernel load balancing)
- HTTP/1.1 and HTTP/2 support
- TLS/HTTPS with ALPN protocol negotiation
- Per-IP rate limiting (token bucket algorithm)
- Tiered connection slots (NORMAL/LARGE/HUGE)
- Slowloris attack protection
- Graceful reload (SIGHUP) and shutdown (SIGTERM)
- Zero-copy buffer handling with libevent

## Requirements

```bash
# Ubuntu/Debian
sudo apt install build-essential libevent-dev libssl-dev libnghttp2-dev pkg-config
```

## Build

```bash
make clean && make
```

## Configuration

```bash
cp config.ini.example config.ini
# Edit config.ini as needed
```

For TLS, generate certificates:
```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes -subj "/CN=localhost"
```

## Run

```bash
# Auto-detect workers (1 per CPU)
./rawrelay-server

# Override worker count
./rawrelay-server -w 4

# Test configuration
./rawrelay-server -t

# Custom config file
./rawrelay-server /path/to/config.ini
```

## Signals

| Signal | Action |
|--------|--------|
| SIGTERM | Graceful shutdown |
| SIGINT | Graceful shutdown |
| SIGHUP | Graceful reload (new workers, drain old) |

## Endpoints

| Path | Response |
|------|----------|
| `/{64-hex-chars}` | result.html (txid lookup) |
| `/tx/{64-hex-chars}` | result.html (txid lookup) |
| `/{164+-hex-chars}` | broadcast.html (raw tx) |
| Other | error.html (400) |

## Ports

| Port | Protocol |
|------|----------|
| 8080 | HTTP/1.1 |
| 8443 | HTTPS (HTTP/1.1 or HTTP/2 via ALPN) |

## Tests

```bash
# Run all tests
bash tests/final_verification.sh

# Memory check
valgrind --leak-check=full ./rawrelay-server -w 1
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Master Process                        │
│  - Forks workers                                        │
│  - Monitors workers (restart on crash)                  │
│  - Handles SIGHUP/SIGTERM                               │
└─────────────────────────┬───────────────────────────────┘
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
┌───────────────┐ ┌───────────────┐ ┌───────────────┐
│   Worker 0    │ │   Worker 1    │ │   Worker N    │
│  CPU Core 0   │ │  CPU Core 1   │ │  CPU Core N   │
│  Own socket   │ │  Own socket   │ │  Own socket   │
│  Own evloop   │ │  Own evloop   │ │  Own evloop   │
│  No locks!    │ │  No locks!    │ │  No locks!    │
└───────────────┘ └───────────────┘ └───────────────┘
```

## License

MIT
