# Testing Infrastructure

This document outlines the testing strategy for the RawRelay server, a production C application handling Bitcoin transaction broadcasts.

## Testing Philosophy

For a security-critical C server, testing follows a layered approach where each layer catches different classes of bugs:

```
                    ┌─────────────────┐
                    │   Fuzz Testing  │  ← Edge cases, security vulnerabilities
                   ┌┴─────────────────┴┐
                   │    Integration     │  ← HTTP flows, TLS, end-to-end paths
                  ┌┴───────────────────┴┐
                  │    Memory Safety    │  ← ASan, Valgrind (critical for C)
                 ┌┴─────────────────────┴┐
                 │   Static Analysis     │  ← Bugs detected without execution
                ┌┴───────────────────────┴┐
                │      Unit Tests         │  ← Individual function correctness
               ┌┴─────────────────────────┴┐
               │      Build Matrix         │  ← Cross-platform compatibility
               └───────────────────────────┘
```

---

## CI/CD Pipeline

### Build Matrix

Every push and pull request triggers builds across multiple platforms and compilers:

| OS | Compilers | Purpose |
|----|-----------|---------|
| Ubuntu 22.04 | gcc, clang | Primary Linux LTS target |
| Ubuntu 24.04 | gcc, clang | Latest Linux LTS |
| macOS 14 | gcc, clang | macOS ARM64 (Apple Silicon) |
| macOS 15 | gcc, clang | Latest macOS |

All builds use strict compiler warnings: `-Wall -Wextra -Werror`

### Workflow Structure

```
.github/workflows/
├── ci.yml           # Build + unit tests + integration (every push/PR)
├── security.yml     # ASan/UBSan + static analysis (every PR)
└── performance.yml  # Benchmark regression (weekly/manual trigger)
```

---

## Test Categories

### 1. Unit Tests

Test individual functions in isolation. Located in `tests/`.

| Test File | Coverage |
|-----------|----------|
| `test_network.c` | Network connectivity, DNS resolution |
| `test_rpc.c` | JSON-RPC request/response handling |
| `test_hex.c` | Hex validation (`is_hex_char()`, `is_all_hex()`) |
| `test_chunk_buffer.c` | Buffer allocation, append, overflow |
| `test_config.c` | Configuration parsing, defaults |
| `test_rate_limiter.c` | Rate limit logic, token bucket, expiry |
| `test_slot_manager.c` | Slot acquire/release, tier promotion |
| `test_ip_acl.c` | IP matching, CIDR parsing, allowlist/blocklist |
| `test_http_parser.c` | Malformed requests, header parsing |

**Run locally:**
```bash
make test
```

### 2. Static Analysis

Detect bugs without executing code.

| Tool | What It Catches |
|------|-----------------|
| Compiler warnings | Type mismatches, unused variables, format strings |
| `cppcheck` | Memory leaks, null dereferences, buffer overflows, dead code |
| `clang-tidy` | Modern C best practices, potential bugs |
| `scan-build` | Deep path-sensitive analysis |

**Run locally:**
```bash
# cppcheck
cppcheck --enable=all --error-exitcode=1 src/ include/

# clang static analyzer
scan-build make clean all
```

### 3. Memory Safety

Critical for C code. Memory bugs are the #1 source of security vulnerabilities.

| Tool | What It Catches | Overhead |
|------|-----------------|----------|
| AddressSanitizer (ASan) | Buffer overflow, use-after-free, double-free | ~2x |
| LeakSanitizer (LSan) | Memory leaks | Minimal |
| UndefinedBehaviorSanitizer (UBSan) | Integer overflow, null pointer, alignment | Minimal |
| Valgrind | All above + uninitialized memory reads | ~20x |

**Run locally:**
```bash
# Build with sanitizers
make clean
make CFLAGS="-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer"

# Run tests (sanitizer will abort on errors)
./rawrelay-server -c config.ini.example -w 1 &
curl http://localhost:8080/health
pkill rawrelay-server

# Valgrind (slower, more thorough)
valgrind --leak-check=full --error-exitcode=1 ./rawrelay-server -t
```

### 4. Integration Tests

Test HTTP request/response flows end-to-end.

| Test Case | What It Verifies |
|-----------|------------------|
| Health endpoint | Server responds, returns valid JSON |
| 64-char hex path | Transaction ID routing works |
| 200-char hex path | Longer hex strings handled correctly |
| Large URLs (10KB+) | Chunk buffer handles large requests |
| Invalid hex | Error page returned, no crash |
| Rate limiting | 429 returned after threshold exceeded |
| Keep-alive | Multiple requests on single connection |
| TLS handshake | HTTPS connections work |
| HTTP/2 | Multiplexed streams work |

**Run locally:**
```bash
./tests/test_integration.sh
```

### 5. Security Tests

Verify resistance to common attack vectors.

| Test | What It Verifies |
|------|------------------|
| Slowloris resistance | Slow/incomplete requests timeout |
| Request size limits | Oversized requests rejected |
| Path traversal | `/../` sequences blocked |
| Header injection | CRLF injection blocked |
| Connection limits | Slot exhaustion returns 503, not crash |

**Run locally:**
```bash
./tests/test_stress.sh
```

### 6. Fuzz Testing

Automated discovery of edge cases and vulnerabilities using random/mutated inputs.

| Tool | Target | Purpose |
|------|--------|---------|
| AFL++ | HTTP parser | Find parsing bugs with malformed requests |
| libFuzzer | `is_all_hex()`, config parser | Find crashes in input handling |

Fuzz testing is run periodically (not on every commit) due to time requirements.

**Run locally:**
```bash
# Requires AFL++ installed
cd fuzz/
make
afl-fuzz -i seeds/ -o findings/ ./fuzz_http_parser
```

### 7. Performance Regression

Prevent performance degradation over time.

| Metric | Baseline | Threshold |
|--------|----------|-----------|
| Health endpoint throughput | 70,000+ req/s | Fail if <60,000 |
| 64-char hex throughput | 90,000+ req/s | Fail if <75,000 |
| Memory usage (idle) | <5 MB | Fail if >10 MB |
| Startup time | <100 ms | Fail if >500 ms |

**Run locally:**
```bash
./rawrelay-server -c config.ini.example -w 2 &
sleep 2
ab -n 10000 -c 50 -k http://localhost:8080/health
pkill rawrelay-server
```

---

## Running the Full Test Suite

### Quick Validation (before commit)
```bash
make clean && make          # Build with strict warnings
make test                   # Unit tests
./tests/test_integration.sh # Integration tests
```

### Full Validation (before release)
```bash
# Clean build
make clean && make

# Unit tests
make test

# Integration tests
./tests/test_integration.sh

# Memory safety (ASan)
make clean
make CFLAGS="-fsanitize=address,undefined -g -O1"
./tests/test_integration.sh

# Static analysis
cppcheck --enable=all --error-exitcode=1 src/ include/

# Valgrind (thorough memory check)
make clean && make
valgrind --leak-check=full --error-exitcode=1 ./rawrelay-server -t

# Performance benchmark
./benchmark.sh
```

---

## Adding New Tests

### Unit Test Template

```c
// tests/test_example.c
#include <assert.h>
#include <stdio.h>
#include "../include/example.h"

static void test_basic_functionality(void) {
    assert(example_function(1) == expected_value);
    printf("  [PASS] test_basic_functionality\n");
}

static void test_edge_cases(void) {
    assert(example_function(0) == 0);
    assert(example_function(-1) == -1);
    printf("  [PASS] test_edge_cases\n");
}

int main(void) {
    printf("Running example tests...\n");
    test_basic_functionality();
    test_edge_cases();
    printf("All tests passed!\n");
    return 0;
}
```

### Integration Test Template

```bash
#!/bin/bash
# tests/test_new_feature.sh

set -e

echo "Testing new feature..."

# Start server
./rawrelay-server -c config.ini.example -w 1 &
SERVER_PID=$!
sleep 2

# Test case
RESPONSE=$(curl -s http://localhost:8080/new-endpoint)
if [[ "$RESPONSE" != *"expected"* ]]; then
    echo "FAIL: Unexpected response"
    kill $SERVER_PID
    exit 1
fi

echo "PASS: New feature works"
kill $SERVER_PID
```

---

## Roadmap

### Implemented
- [x] Multi-OS build matrix (Ubuntu, macOS)
- [x] Multi-compiler testing (gcc, clang)
- [x] Unit tests (network, RPC)
- [x] Basic integration tests
- [x] Strict compiler warnings

### In Progress
- [ ] AddressSanitizer CI job
- [ ] Static analysis CI job (cppcheck)
- [ ] Expanded unit test coverage

### Planned
- [ ] Fuzz testing infrastructure
- [ ] Performance regression CI
- [ ] Code coverage reporting
- [ ] Security-focused test suite

---

## Contributing

When submitting changes:

1. Ensure `make` succeeds with no warnings
2. Run `make test` and verify all tests pass
3. Add tests for new functionality
4. For security-sensitive changes, run ASan build locally

CI will automatically verify your changes across all supported platforms.
