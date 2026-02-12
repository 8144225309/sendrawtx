# Deployment Guide

This walks through deploying sendrawtx from source to a running production service with TLS. For tuning, signals, metrics, and security hardening, see [OPERATIONS.md](OPERATIONS.md).

**Tested on:** Ubuntu 22.04 / 24.04, macOS 14 / 15 (CI-tested). Other Linux distributions work with minor package-name adjustments.

---

## 1. Install Dependencies

**Linux (Ubuntu/Debian):**

```bash
sudo apt update
sudo apt install -y build-essential libevent-dev libssl-dev libnghttp2-dev pkg-config
```

**macOS:**

```bash
brew install libevent openssl nghttp2 pkg-config
```

## 2. Build

```bash
git clone https://github.com/8144225309/sendrawtx.git
cd sendrawtx
make
```

Verify the build:

```bash
./rawrelay-server -t
```

This parses the default config and exits. You should see the parsed configuration printed with no errors.

## 3. Configure

```bash
cp config.ini.example config.ini
```

### Network and RPC

Edit `config.ini` to set your chain and connect to your Bitcoin node. RPC is configured per-chain using `[rpc.<chain>]` sections:

```ini
[network]
chain = mainnet

[rpc.mainnet]
enabled = 1
host = 127.0.0.1
port = 8332
cookie_file = /home/bitcoin/.bitcoin/.cookie
timeout = 30
```

**Authentication** — use one of:

| Method | Config | Notes |
|--------|--------|-------|
| Cookie file | `cookie_file = /path/to/.cookie` | Preferred. Auto-rotated by Bitcoin Core |
| Datadir | `datadir = /home/bitcoin/.bitcoin` | Auto-discovers the cookie file |
| Username/password | `user = ...` / `password = ...` | Static credentials |

**Multiple nodes:** You can enable multiple chains simultaneously. Each gets its own RPC client with independent connection tracking:

```ini
[rpc.mainnet]
enabled = 1
host = 127.0.0.1
port = 8332
cookie_file = /home/bitcoin/.bitcoin/.cookie

[rpc.testnet]
enabled = 1
host = 127.0.0.1
port = 18332
cookie_file = /home/bitcoin/.bitcoin/testnet3/.cookie

[rpc.signet]
enabled = 1
host = 127.0.0.1
port = 38332
cookie_file = /home/bitcoin/.bitcoin/signet/.cookie
```

**Mixed mode:** Set `chain = mixed` to route transactions automatically based on address prefix (`bc1...` to mainnet, `tb1...` to testnet, `bcrt1...` to regtest, etc.). See `config.mixed.ini` for a complete example.

**Default RPC ports:** 8332 (mainnet), 18332 (testnet), 38332 (signet), 18443 (regtest).

### Test the config

```bash
./rawrelay-server -t
```

If RPC is configured, the output will show the connection details. Fix any errors before proceeding.

## 4. Install as Service (Linux)

The install script creates a `rawrelay` system user, copies files to `/opt/rawrelay`, and installs the systemd unit:

```bash
make
sudo make install
```

This runs `contrib/install.sh`, which:
- Creates the `rawrelay` user (no login shell)
- Installs the binary to `/opt/rawrelay/rawrelay-server`
- Copies `config.ini.example` to `/opt/rawrelay/config.ini`
- Creates `/opt/rawrelay/.well-known/acme-challenge/` for ACME
- Installs `contrib/init/rawrelay.service` to systemd

**Edit the installed config** with your RPC settings:

```bash
sudo nano /opt/rawrelay/config.ini
```

**Start and enable:**

```bash
sudo systemctl start rawrelay
sudo systemctl enable rawrelay
sudo systemctl status rawrelay
```

**Logs:**

```bash
sudo journalctl -u rawrelay -f
```

### macOS (launchd)

macOS uses launchd instead of systemd. To run manually:

```bash
./rawrelay-server config.ini
./rawrelay-server -w 4 config.ini   # explicit worker count
```

To run as a background service, create a launchd plist:

```bash
sudo tee /Library/LaunchDaemons/com.rawrelay.server.plist << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.rawrelay.server</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/rawrelay-server</string>
        <string>/usr/local/etc/rawrelay/config.ini</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/var/log/rawrelay.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/rawrelay.log</string>
</dict>
</plist>
EOF
```

```bash
# Copy files into place
sudo mkdir -p /usr/local/etc/rawrelay
sudo cp rawrelay-server /usr/local/bin/
sudo cp config.ini /usr/local/etc/rawrelay/

# Load and start
sudo launchctl load /Library/LaunchDaemons/com.rawrelay.server.plist
sudo launchctl start com.rawrelay.server
```

**Note:** Seccomp is Linux-only and has no effect on macOS. The `SO_REUSEPORT` multi-process model works on both platforms.

## 5. TLS with Let's Encrypt

sendrawtx has a built-in ACME HTTP-01 challenge endpoint. No reverse proxy needed — the server handles TLS termination directly.

### Initial certificate

The server must be reachable on port 8080 for the HTTP-01 challenge. Install certbot and request a certificate:

```bash
sudo apt install -y certbot

sudo certbot certonly --webroot \
  -w /opt/rawrelay \
  -d yourdomain.com
```

Certbot writes challenge tokens to `/opt/rawrelay/.well-known/acme-challenge/`, which the server serves automatically at `http://yourdomain.com:8080/.well-known/acme-challenge/{token}`.

**Multiple domains:** If you serve multiple domains from the same server (e.g. both `sendrawtx.com` and `sendrawtransaction.com` pointing to the same IP), request a single SAN certificate covering all of them:

```bash
sudo certbot certonly --webroot \
  -w /opt/rawrelay \
  -d sendrawtx.com \
  -d sendrawtransaction.com
```

One cert file covers both domains. The server has no SNI support (it loads a single certificate), but a SAN cert means both domains validate. The ACME challenge endpoint is domain-agnostic — it serves tokens based on the URL path regardless of Host header, so validation succeeds for all domains in the cert.

### Enable TLS in config

Edit `/opt/rawrelay/config.ini`:

```ini
[tls]
enabled = 1
port = 8443
cert_file = /etc/letsencrypt/live/yourdomain.com/fullchain.pem
key_file = /etc/letsencrypt/live/yourdomain.com/privkey.pem
http2_enabled = 1
```

The `rawrelay` user needs read access to the certificate files:

```bash
sudo chmod 750 /etc/letsencrypt/live/
sudo chmod 750 /etc/letsencrypt/archive/
sudo chgrp rawrelay /etc/letsencrypt/live/ /etc/letsencrypt/archive/
```

Reload the service:

```bash
sudo systemctl restart rawrelay
```

### Auto-renewal

Certbot renews automatically via its systemd timer. After renewal, the server needs to reload the new certificate. Create a deploy hook:

```bash
sudo tee /etc/letsencrypt/renewal-hooks/deploy/rawrelay-reload.sh << 'EOF'
#!/bin/bash
kill -USR2 $(pgrep rawrelay-server)
EOF
sudo chmod +x /etc/letsencrypt/renewal-hooks/deploy/rawrelay-reload.sh
```

SIGUSR2 reloads the TLS certificate from disk without dropping connections. New connections get the new cert; existing connections continue with the old one until they close naturally.

Test the renewal flow:

```bash
sudo certbot renew --dry-run
```

## 6. Port Forwarding (80/443 to 8080/8443)

### Why not a reverse proxy?

sendrawtx accepts raw transaction hex directly in the URL path — URLs can be up to 4 MB. Reverse proxies (nginx, HAProxy, Caddy) impose URL length limits well below this and will silently truncate or reject requests. **Do not put a reverse proxy in front of sendrawtx.**

### Linux (iptables)

Use iptables to forward standard ports to the server's ports without any proxy layer:

```bash
# HTTP: 80 → 8080
sudo iptables -t nat -A PREROUTING -p tcp --dport 80 -j REDIRECT --to-port 8080

# HTTPS: 443 → 8443
sudo iptables -t nat -A PREROUTING -p tcp --dport 443 -j REDIRECT --to-port 8443
```

Make it persistent:

```bash
sudo apt install -y iptables-persistent
sudo netfilter-persistent save
```

This saves the rules to `/etc/iptables/rules.v4` and restores them on boot.

Verify:

```bash
sudo iptables -t nat -L PREROUTING -n --line-numbers
```

You should see the two REDIRECT rules. Traffic on ports 80 and 443 now reaches the server on 8080 and 8443 respectively.

### macOS (pf)

macOS uses `pf` (packet filter) instead of iptables:

```bash
# Add redirect rules
echo "rdr pass on lo0 inet proto tcp from any to any port 80 -> 127.0.0.1 port 8080
rdr pass on lo0 inet proto tcp from any to any port 443 -> 127.0.0.1 port 8443" | sudo tee -a /etc/pf.conf

# Reload pf
sudo pfctl -f /etc/pf.conf
sudo pfctl -e
```

For external traffic, replace `lo0` with your network interface (e.g. `en0`).

**Note:** The ACME HTTP-01 challenge must be reachable on port 80. With port forwarding in place, certbot's validation requests to `http://yourdomain.com/.well-known/acme-challenge/{token}` (port 80) will reach the server on port 8080 automatically.

## 7. Verify

### Health check

```bash
curl -s http://localhost:8080/health | python3 -m json.tool
```

Check that `status` is `"healthy"` and TLS fields show your cert expiry if TLS is enabled.

### Readiness and liveness

```bash
curl -sf http://localhost:8080/ready && echo "ready"
curl -sf http://localhost:8080/alive && echo "alive"
```

### Test a broadcast (regtest)

If you have a regtest node, generate a raw transaction and broadcast it:

```bash
# Create and fund a wallet on your regtest node
bitcoin-cli -regtest createwallet "test"
bitcoin-cli -regtest generatetoaddress 101 $(bitcoin-cli -regtest getnewaddress)

# Create a raw transaction
ADDR=$(bitcoin-cli -regtest getnewaddress)
RAWTX=$(bitcoin-cli -regtest -named createrawtransaction inputs='[]' outputs="{\"$ADDR\":0.001}" | xargs bitcoin-cli -regtest fundrawtransaction | python3 -c "import sys,json; print(json.load(sys.stdin)['hex'])" | xargs -I{} bitcoin-cli -regtest signrawtransactionwithwallet {} | python3 -c "import sys,json; print(json.load(sys.stdin)['hex'])")

# Broadcast via sendrawtx
curl "http://localhost:8080/${RAWTX}"
```

Tip: test with a regtest configuration (`config.regtest.ini`) before switching to mainnet.

### Metrics

```bash
curl -s http://localhost:8080/metrics | head -20
```

## 8. Monitoring

### Prometheus

Add a scrape target for each server instance:

```yaml
scrape_configs:
  - job_name: 'rawrelay'
    scrape_interval: 15s
    static_configs:
      - targets: ['localhost:8080']
```

Key metrics to alert on:
- `rawrelay_tls_cert_expiry_timestamp_seconds` — alert when < 7 days
- `rawrelay_connections_rejected_total{reason="slot_limit"}` — capacity pressure
- `rawrelay_connections_rejected_total{reason="rate_limit"}` — abuse detection
- `rawrelay_http_requests_by_class_total{class="5xx"}` — server errors

See [OPERATIONS.md](OPERATIONS.md) for the full metrics reference.

### Log rotation

Logs go to journald via the systemd service. Journald handles rotation automatically. To configure retention:

```bash
sudo nano /etc/systemd/journald.conf
```

```ini
[Journal]
SystemMaxUse=500M
MaxRetentionSec=30day
```

```bash
sudo systemctl restart systemd-journald
```

## Troubleshooting

**Service won't start — config error**
```bash
# Test config in place
sudo -u rawrelay /opt/rawrelay/rawrelay-server -t
```

**Permission denied on certificate files**
```bash
# Verify the rawrelay user can read the certs
sudo -u rawrelay cat /etc/letsencrypt/live/yourdomain.com/fullchain.pem > /dev/null
```

**Port 8080 already in use**
```bash
sudo ss -tlnp | grep 8080
```

**ACME challenge failing**
```bash
# Verify the challenge directory exists and is writable
ls -la /opt/rawrelay/.well-known/acme-challenge/
# Test that the server serves the path
echo "test" | sudo -u rawrelay tee /opt/rawrelay/.well-known/acme-challenge/test-token
curl http://localhost:8080/.well-known/acme-challenge/test-token
sudo rm /opt/rawrelay/.well-known/acme-challenge/test-token
```

**RPC connection refused**
```bash
# Check Bitcoin Core is running and listening
bitcoin-cli getnetworkinfo
# Verify the cookie file is readable by rawrelay
sudo -u rawrelay cat /home/bitcoin/.bitcoin/.cookie
```

**iptables rules lost after reboot**
```bash
sudo netfilter-persistent save
sudo systemctl enable netfilter-persistent
```

**Workers crashing with seccomp**

Seccomp violations kill the worker silently. Disable seccomp first (`seccomp = 0` in config), confirm the server works, then re-enable. See [OPERATIONS.md](OPERATIONS.md#seccomp) for details.
