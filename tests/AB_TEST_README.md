# Performance A/B Test for PR #2

This test verifies the performance optimizations in the `perf-optimizations` branch.

## Quick Start (macOS)

```bash
# 1. Install dependencies
brew install libevent openssl nghttp2 wrk

# 2. Run the test
cd /path/to/sendrawtx
bash tests/ab_test_performance.sh
```

The test takes ~5 minutes and produces a report in `ab_test_results/REPORT.md`.

## What's Being Tested

### 1. Hex Lookup Table
**Before:** 3 comparisons per character
```c
if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
```

**After:** 1 table lookup per character
```c
if (hex_lookup_table[c])  // O(1)
```

**Expected improvement:** 10-30% faster on hex path validation (txid, rawtx endpoints)

### 2. RPC Growing Buffer
**Before:** Allocate 4MB upfront for every RPC call
```c
char *buffer = malloc(4 * 1024 * 1024);  // 4MB always
```

**After:** Start small, grow only if needed
```c
char *buffer = malloc(64 * 1024);  // 64KB initial
// ... grows by 2x only when needed
```

**Expected improvement:** 98% memory reduction for typical RPC calls

### 3. Chunk Buffer
**Before:** realloc() on every large URL growth (causes copies)

**After:** Linked list of 64KB chunks (no copying)

**Expected improvement:** Faster handling of large URLs (1KB+), less memory fragmentation

## Test Cases

| Test | Path | What It Measures |
|------|------|------------------|
| health | `/health` | Baseline throughput |
| txid-64 | `/0000...0000` (64 chars) | Hex validation (small) |
| rawtx-200 | `/0000...0000` (200 chars) | Hex validation (medium) |
| largetx-1000 | `/aaaa...aaaa` (1000 chars) | Chunk buffer (large) |

## Expected Results

```
Test                      Baseline      Optimized     Change
----                      --------      ---------     ------
health                    15000 rps     15000 rps     +0%      (no change expected)
txid-64                   12000 rps     15000 rps     +25%     (hex LUT improvement)
rawtx-200                 10000 rps     12000 rps     +20%     (hex LUT improvement)
largetx-1000              8000 rps      9000 rps      +12%     (chunk buffer)
```

Actual numbers will vary by system. Look for:
- **txid-64 and rawtx-200**: Should show 10-30% improvement
- **health**: Should be roughly the same (no hex validation)
- **Memory**: Should be lower or equal

## Pass/Fail Criteria

**PASS** if:
- All tests show >= -5% change (no significant regression)
- At least one hex test shows >= 10% improvement

**FAIL** if:
- Any test shows > 5% regression
- Memory usage significantly increases

## Troubleshooting

**wrk not found:**
```bash
brew install wrk
```

**Port 8080 in use:**
```bash
lsof -i :8080
kill <PID>
```

**Build fails:**
```bash
brew install libevent openssl nghttp2 pkg-config
```

## Output Files

After running, check `ab_test_results/`:
- `REPORT.md` - Summary report
- `baseline-*.txt` - Detailed wrk output for main branch
- `optimized-*.txt` - Detailed wrk output for perf-optimizations
- `*-memory.txt` - Memory measurements
- `*-server.log` - Server logs (useful for debugging)
