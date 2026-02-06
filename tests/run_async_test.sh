#!/usr/bin/env bash
#
# Starts a fresh regtest bitcoind, funds a wallet, creates a real
# signed transaction, runs the async RPC test binary, then cleans up.
#
# Usage:
#   ./tests/run_async_test.sh ./build/test_async_rpc
#
# Environment overrides:
#   BITCOIND      path to bitcoind        (default: auto-detect)
#   BITCOIN_CLI   path to bitcoin-cli     (default: auto-detect)
#   RPC_PORT      port for regtest node   (default: 18600)

set -euo pipefail

TEST_BIN="${1:?Usage: $0 <path-to-test-binary>}"

# Auto-detect bitcoin binaries
if [ -z "${BITCOIND:-}" ]; then
    for p in \
        /home/obscurity/bitcoin-30.2/bin/bitcoind \
        /home/obscurity/bitcoin-28.0/bin/bitcoind \
        /usr/local/bin/bitcoind \
        /usr/bin/bitcoind; do
        [ -x "$p" ] && BITCOIND="$p" && break
    done
fi
if [ -z "${BITCOIN_CLI:-}" ]; then
    BITCOIN_CLI="$(dirname "$BITCOIND")/bitcoin-cli"
fi

if [ ! -x "$BITCOIND" ]; then
    echo "ERROR: bitcoind not found. Set BITCOIND env var." >&2
    exit 1
fi

PORT="${RPC_PORT:-18600}"
USER="asynctest"
PASS="asynctest$(date +%s)"
DATADIR=$(mktemp -d)

echo "bitcoind: $BITCOIND"
echo "datadir:  $DATADIR"
echo "port:     $PORT"

# Cleanup on exit
cleanup() {
    echo ""
    echo "Cleaning up..."
    "$BITCOIN_CLI" -datadir="$DATADIR" -rpcport="$PORT" \
        -rpcuser="$USER" -rpcpassword="$PASS" stop 2>/dev/null || true
    # Wait for bitcoind to actually exit
    for i in $(seq 1 20); do
        "$BITCOIN_CLI" -datadir="$DATADIR" -rpcport="$PORT" \
            -rpcuser="$USER" -rpcpassword="$PASS" \
            getblockchaininfo >/dev/null 2>&1 || break
        sleep 0.5
    done
    rm -rf "$DATADIR"
    echo "Done."
}
trap cleanup EXIT

# Start bitcoind
echo "Starting bitcoind regtest..."
"$BITCOIND" -regtest -daemon \
    -datadir="$DATADIR" \
    -rpcport="$PORT" \
    -rpcuser="$USER" \
    -rpcpassword="$PASS" \
    -fallbackfee=0.0001 \
    -txindex=1 \
    -rpcallowip=127.0.0.1 \
    -rpcbind=127.0.0.1

# Wait for it
echo -n "Waiting for node..."
for i in $(seq 1 60); do
    if "$BITCOIN_CLI" -datadir="$DATADIR" -rpcport="$PORT" \
        -rpcuser="$USER" -rpcpassword="$PASS" \
        getblockchaininfo >/dev/null 2>&1; then
        echo " ready."
        break
    fi
    echo -n "."
    sleep 0.5
done

# Verify it's actually running
if ! "$BITCOIN_CLI" -datadir="$DATADIR" -rpcport="$PORT" \
    -rpcuser="$USER" -rpcpassword="$PASS" \
    getblockchaininfo >/dev/null 2>&1; then
    echo " FAILED — bitcoind did not start." >&2
    exit 1
fi

CLI="$BITCOIN_CLI -datadir=$DATADIR -rpcport=$PORT -rpcuser=$USER -rpcpassword=$PASS"

# Create wallet and mine blocks
echo "Creating wallet and mining 101 blocks..."
$CLI createwallet "test" >/dev/null 2>&1

ADDR=$($CLI getnewaddress)
$CLI generatetoaddress 101 "$ADDR" >/dev/null 2>&1

BALANCE=$($CLI getbalance)
echo "Wallet balance: $BALANCE BTC"

# Create a real signed transaction
echo "Creating funded+signed transaction..."
DEST=$($CLI getnewaddress)
RAW=$($CLI createrawtransaction '[]' "{\"$DEST\":0.001}")
FUNDED=$($CLI fundrawtransaction "$RAW" | jq -r '.hex')
SIGNED=$($CLI signrawtransactionwithwallet "$FUNDED" | jq -r '.hex')

echo "TX hex: ${SIGNED:0:60}... (${#SIGNED} chars)"
echo ""

# Run the test
echo "================================================"
echo "Running: $TEST_BIN 127.0.0.1 $PORT $USER $PASS <tx>"
echo "================================================"
echo ""

${VALGRIND:-} "$TEST_BIN" 127.0.0.1 "$PORT" "$USER" "$PASS" "$SIGNED"
EXIT_CODE=$?

echo ""
echo "Test exited with code: $EXIT_CODE"
exit $EXIT_CODE
