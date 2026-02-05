# RawRelay v6 → Production: Complete Phase Plan

## Current State Summary

### What We've Built (COMPLETE)

| Component | Lines | Status |
|-----------|-------|--------|
| Phase 1: Multi-process foundation | ~600 | ✅ |
| Phase 2: Request handling + routing | ~900 | ✅ |
| Phase 3: SlotManager (connection limits) | ~100 | ✅ |
| Phase 4: Rate Limiter (abuse protection) | ~200 | ✅ |
| v6 Phase 1: evbuffer O(n) fix | ~150 | ✅ |
| v6 Phase 2: TLS/HTTPS + ALPN | ~200 | ✅ |
| v6 Phase 3: HTTP/2 + nghttp2 | ~600 | ✅ |
| v6 Phase 4: Stream-level slots | ~50 | ✅ |
| Keep-alive support | ~100 | ✅ |
| Slowloris protection | ~50 | ✅ |
| Early path validation | ~100 | ✅ |

**Total production code: ~3,050 lines**

### What's Missing (TO BUILD)

```
┌────────────────────────────────────────────────────────────────────────┐
│                        CURRENT ARCHITECTURE                             │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────────────────┐  │
│  │ Accept  │───▶│  Rate   │───▶│  Slot   │───▶│  Static File Serve  │  │
│  │ Socket  │    │ Limiter │    │ Manager │    │  (broadcast.html)   │  │
│  └─────────┘    └─────────┘    └─────────┘    └─────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ MISSING
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                      REQUIRED ARCHITECTURE                              │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐                             │
│  │ Accept  │───▶│Blocklist│───▶│  Rate   │───┐                         │
│  │ Socket  │    │ Check   │    │ Limiter │   │                         │
│  └─────────┘    └─────────┘    └─────────┘   │                         │
│                                              ▼                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                        ROUTING                                   │   │
│  │  /health  /metrics  /tx/{txid}  /{rawtx}  /.well-known/acme/    │   │
│  └──────┬────────┬─────────┬──────────┬─────────────┬──────────────┘   │
│         │        │         │          │             │                   │
│         ▼        ▼         ▼          ▼             ▼                   │
│    ┌────────┐ ┌───────┐ ┌──────┐ ┌─────────────┐ ┌──────────┐         │
│    │ Health │ │Metrics│ │Result│ │  Validate   │ │  ACME    │         │
│    │ 200 OK │ │ Prom  │ │ JSON │ │  Broadcast  │ │ Challenge│         │
│    └────────┘ └───────┘ └──────┘ └──────┬──────┘ └──────────┘         │
│                            │            │                               │
│                            ▼            ▼                               │
│                     ┌──────────┐  ┌──────────┐                         │
│                     │  Result  │  │ Bitcoin  │                         │
│                     │  Store   │◀─│  Nodes   │                         │
│                     │(SQLite)  │  │  (RPC)   │                         │
│                     └──────────┘  └──────────┘                         │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Phase Definitions

### Phase 5: Observability & Operations
**Goal:** Make the server production-deployable with proper monitoring

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 5.1 | `/health`, `/ready`, `/alive` endpoints | ~80 | CRITICAL |
| 5.2 | `/metrics` endpoint (Prometheus format) | ~150 | CRITICAL |
| 5.3 | JSON structured logging option | ~100 | HIGH |
| 5.4 | Access logging (request/response log) | ~80 | HIGH |
| 5.5 | X-Request-ID header generation | ~40 | MEDIUM |
| 5.6 | Request duration tracking | ~50 | MEDIUM |

**Deliverables:**
- Load balancer can health-check the server
- Prometheus can scrape metrics
- Logs can be ingested into ELK/Loki/CloudWatch

---

### Phase 6: IP Management & Security
**Goal:** Persistent ban/allow lists beyond rate limiting

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 6.1 | IP blocklist file support (`/etc/rawrelay/blocklist.txt`) | ~100 | HIGH |
| 6.2 | IP allowlist file (bypass rate limiting) | ~60 | MEDIUM |
| 6.3 | CIDR block support (e.g., `192.168.0.0/16`) | ~80 | MEDIUM |
| 6.4 | Hot-reload blocklist on SIGHUP | ~40 | MEDIUM |
| 6.5 | Auto-ban on repeated abuse (N violations → ban) | ~100 | LOW |
| 6.6 | GeoIP blocking (optional, libmaxminddb) | ~150 | LOW |

**Deliverables:**
- Persistent IP blocking
- Easy ops management via files
- Protection against known bad actors

---

### Phase 7: Transaction Validation (Premature Verification)
**Goal:** Reject invalid transactions BEFORE broadcasting

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 7.1 | Hex decode to raw bytes | ~50 | CRITICAL |
| 7.2 | Transaction structure validation (version, inputs, outputs) | ~200 | CRITICAL |
| 7.3 | Size limits enforcement (min/max tx size) | ~30 | HIGH |
| 7.4 | Output script validation (valid opcodes) | ~150 | MEDIUM |
| 7.5 | Input signature presence check | ~80 | MEDIUM |
| 7.6 | (Optional) Full script validation with libbitcoin | ~300+ | LOW |
| 7.7 | (Optional) Signature verification with libsecp256k1 | ~200+ | LOW |

**Deliverables:**
- Invalid hex rejected immediately (400)
- Malformed transactions rejected (400)
- Size violations rejected (400)

**Note:** Bitcoin-specific validation rules to be provided separately.

---

### Phase 8: Backend Integration (Broadcast Logic)
**Goal:** Actually broadcast transactions to Bitcoin network

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 8.1 | HTTP client for external APIs (libcurl or libevent http) | ~200 | CRITICAL |
| 8.2 | Bitcoin Core RPC client (`sendrawtransaction`) | ~150 | CRITICAL |
| 8.3 | Mempool.space API client | ~100 | HIGH |
| 8.4 | Blockstream API client | ~100 | HIGH |
| 8.5 | Electrum server client (optional) | ~200 | MEDIUM |
| 8.6 | Multi-backend broadcast queue | ~150 | HIGH |
| 8.7 | Async broadcast with callbacks | ~100 | HIGH |
| 8.8 | Retry logic with exponential backoff | ~80 | MEDIUM |

**Deliverables:**
- Transactions actually broadcast to network
- Multiple backend support for redundancy
- Async processing (don't block HTTP response)

---

### Phase 9: Result Storage & Polling API
**Goal:** Store results for client polling

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 9.1 | SQLite storage for broadcast results | ~200 | CRITICAL |
| 9.2 | `GET /tx/{txid}` JSON API | ~100 | CRITICAL |
| 9.3 | Result expiration/cleanup (TTL) | ~60 | HIGH |
| 9.4 | Status states: pending, broadcast, confirmed, failed | ~50 | HIGH |
| 9.5 | Per-backend result tracking | ~80 | MEDIUM |
| 9.6 | Redis caching layer (optional) | ~150 | LOW |

**Deliverables:**
- Clients can poll for broadcast status
- JSON API returns structured results
- Stale results automatically cleaned

---

### Phase 10: ACME & Certificate Management
**Goal:** Automatic Let's Encrypt integration

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 10.1 | `/.well-known/acme-challenge/` route handler | ~60 | HIGH |
| 10.2 | Challenge file serving from directory | ~40 | HIGH |
| 10.3 | Certificate hot-reload on SIGUSR2 | ~80 | MEDIUM |
| 10.4 | Certificate expiry warning in /health | ~40 | MEDIUM |
| 10.5 | Document certbot integration | ~doc | HIGH |

**Deliverables:**
- Let's Encrypt works with webroot mode
- Certificates reload without restart
- Ops documentation complete

---

### Phase 11: Deployment Artifacts
**Goal:** Production deployment ready

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 11.1 | Systemd service file | ~50 | CRITICAL |
| 11.2 | Systemd hardening (ProtectSystem, etc.) | ~30 | HIGH |
| 11.3 | Dockerfile (multi-stage build) | ~60 | HIGH |
| 11.4 | docker-compose.yml | ~40 | HIGH |
| 11.5 | Kubernetes manifests (Deployment, Service, Ingress) | ~100 | MEDIUM |
| 11.6 | Helm chart (optional) | ~200 | LOW |
| 11.7 | Ansible playbook (optional) | ~150 | LOW |

**Deliverables:**
- One-command deployment
- Container support
- Cloud-native ready

---

### Phase 12: Security Hardening
**Goal:** Production security audit passed

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 12.1 | File descriptor limits handling | ~30 | HIGH |
| 12.2 | Seccomp filter (syscall whitelist) | ~150 | MEDIUM |
| 12.3 | Fuzz testing with AFL | ~doc | HIGH |
| 12.4 | Security audit checklist | ~doc | HIGH |
| 12.5 | Dependency audit (CVE check) | ~doc | MEDIUM |
| 12.6 | Rate limit bypass testing | ~tests | MEDIUM |

**Deliverables:**
- Hardened production deployment
- Documented security posture
- Fuzz testing integrated

---

### Phase 13: Performance Tuning
**Goal:** Maximize throughput, minimize latency

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 13.1 | Connection pooling for backends | ~100 | HIGH |
| 13.2 | Memory pool for frequent allocations | ~150 | MEDIUM |
| 13.3 | Lock removal audit (already lock-free) | ~audit | MEDIUM |
| 13.4 | Benchmark suite | ~tests | HIGH |
| 13.5 | Profile-guided optimization | ~doc | LOW |

**Deliverables:**
- Benchmark numbers documented
- Performance regression tests
- Optimization recommendations

---

### Phase 14: Webhooks & Events (STRETCH GOAL)
**Goal:** External system integration

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 14.1 | Webhook dispatch on broadcast | ~150 | LOW |
| 14.2 | Webhook retry with backoff | ~80 | LOW |
| 14.3 | Redis pub/sub integration | ~100 | LOW |
| 14.4 | Event types: broadcast, confirmed, failed | ~50 | LOW |

**Deliverables:**
- External systems notified of events
- Integration with notification services

---

### Phase 15: Platform Abstraction (FINAL STRETCH GOAL)
**Goal:** Generic platform for multiple use cases

| Job | Description | Lines Est. | Priority |
|-----|-------------|------------|----------|
| 15.1 | Plugin architecture design | ~design | STRETCH |
| 15.2 | Route handler plugins | ~200+ | STRETCH |
| 15.3 | Validator plugins | ~200+ | STRETCH |
| 15.4 | Backend plugins | ~200+ | STRETCH |
| 15.5 | Configuration-driven routing | ~150 | STRETCH |

**Deliverables:**
- Configurable for different use cases
- Plugin-based extensibility

---

## Complete Feature Matrix

| Feature | nginx | HAProxy | Envoy | RawRelay (Current) | RawRelay (Target) |
|---------|:-----:|:-------:|:-----:|:------------------:|:-----------------:|
| Event-driven | ✅ | ✅ | ✅ | ✅ | ✅ |
| Multi-process | ✅ | ✅ | ❌ | ✅ | ✅ |
| SO_REUSEPORT | ✅ | ✅ | ❌ | ✅ | ✅ |
| TLS 1.2+ | ✅ | ✅ | ✅ | ✅ | ✅ |
| HTTP/2 | ✅ | ✅ | ✅ | ✅ | ✅ |
| Rate limiting | ✅ | ✅ | ✅ | ✅ | ✅ |
| Connection limits | ✅ | ✅ | ✅ | ✅ | ✅ |
| Slowloris protection | ⚠️ | ✅ | ✅ | ✅ | ✅ |
| Graceful reload | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Health endpoints** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 5 |
| **Metrics (Prometheus)** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 5 |
| **Access logging** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 5 |
| **Request tracing** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 5 |
| **IP blocklist** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 6 |
| **IP allowlist** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 6 |
| **ACME/Let's Encrypt** | 💰 | ❌ | ❌ | ❌ | ✅ Phase 10 |
| **Systemd service** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 11 |
| **Docker support** | ✅ | ✅ | ✅ | ❌ | ✅ Phase 11 |
| **Seccomp hardening** | ⚠️ | ⚠️ | ✅ | ❌ | ✅ Phase 12 |
| HTTP/3 (QUIC) | ✅ | ⚠️ | ✅ | ❌ | ❌ Future |
| WebSockets | ✅ | ✅ | ✅ | ❌ | ❌ Future |

---

## Execution Order

```
                     PHASE 5: OBSERVABILITY
                            │
                            ▼
               ┌────────────────────────────┐
               │  /health  /metrics  logs   │
               └────────────┬───────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
        ▼                   ▼                   ▼
   PHASE 6             PHASE 10            PHASE 11
   IP Mgmt             ACME/TLS           Deployment
        │                   │                   │
        └───────────────────┼───────────────────┘
                            │
                            ▼
               ┌────────────────────────────┐
               │     PHASE 7: VALIDATION     │
               │   (Bitcoin-specific rules)  │
               └────────────┬───────────────┘
                            │
                            ▼
               ┌────────────────────────────┐
               │     PHASE 8: BACKEND        │
               │   (Bitcoin node comms)      │
               └────────────┬───────────────┘
                            │
                            ▼
               ┌────────────────────────────┐
               │   PHASE 9: RESULT STORE     │
               │   (SQLite + Polling API)    │
               └────────────┬───────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
        ▼                   ▼                   ▼
   PHASE 12            PHASE 13           PHASE 14
   Security             Perf             Webhooks
   Hardening           Tuning            (STRETCH)
        │                   │                   │
        └───────────────────┼───────────────────┘
                            │
                            ▼
               ┌────────────────────────────┐
               │  PHASE 15: ABSTRACTION      │
               │  (FINAL STRETCH GOAL)       │
               └────────────────────────────┘
```

---

## Lines of Code Estimate

| Phase | Description | Estimated Lines |
|-------|-------------|-----------------|
| 5 | Observability | ~500 |
| 6 | IP Management | ~530 |
| 7 | TX Validation | ~510-1010+ |
| 8 | Backend Integration | ~1080 |
| 9 | Result Storage | ~640 |
| 10 | ACME/TLS | ~220 |
| 11 | Deployment | ~480 |
| 12 | Security | ~180+ tests/docs |
| 13 | Performance | ~250+ tests/docs |
| 14 | Webhooks (stretch) | ~380 |
| 15 | Abstraction (stretch) | ~750+ |
| **Total Core (5-11)** | | **~3,960** |
| **Total with Stretch** | | **~5,470+** |

Current codebase: ~3,050 lines
Target codebase: ~7,000-8,500 lines

---

## Input Required Before Implementation

### For Phase 7 (Transaction Validation):
1. Minimum valid transaction size (bytes)
2. Maximum valid transaction size (bytes)
3. Required transaction structure (version, inputs, outputs format)
4. Validation level desired:
   - Level 1: Hex decode + size check only
   - Level 2: + Structure validation (has version, inputs, outputs)
   - Level 3: + Script validation (valid opcodes)
   - Level 4: + Signature verification (requires libsecp256k1)

### For Phase 8 (Backend Integration):
1. Which Bitcoin nodes/APIs to integrate?
   - Bitcoin Core RPC (local node)
   - Mempool.space API
   - Blockstream API
   - Others?
2. RPC credentials handling (config file vs env vars)
3. Retry policy (how many attempts, backoff timing)

---

## Version History

| Date | Version | Changes |
|------|---------|---------|
| 2026-02-05 | 1.0 | Initial roadmap created |

