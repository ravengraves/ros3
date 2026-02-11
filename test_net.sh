#!/bin/bash
# test_net.sh - Test for liaison network bridging
# Publisher -> src -> rosenet_tx -> TCP -> rosenet_rx -> dst -> Subscriber

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'



pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; cleanup; exit 1; }

# Use relative topics
SRC_TOPIC="net/src"
DST_TOPIC="net/dst"
PORT=19999

cleanup() {
    trap - EXIT INT TERM
    kill $PID_PUB $PID_TX $PID_RX 2>/dev/null || true
    rm -f /dev/shm/test1%* /dev/shm/test2%* 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Clean leftover shared memory
rm -f /dev/shm/test1%* /dev/shm/test2%* 2>/dev/null || true

export ROSE_BASE="test1"

# Launch publisher (creates src topic)
"$BUILD_DIR/test_net_pub" "$SRC_TOPIC" 50 >/dev/null 2>&1 &
PID_PUB=$!

sleep 0.1

# Launch transmitter (subscribes to src, sends to rx)
"$BUILD_DIR/rosenet_tx" "$SRC_TOPIC" 127.0.0.1 $PORT >/dev/null 2>&1 &
PID_TX=$!

sleep 0.5

export ROSE_BASE="test2"

# Launch receiver (creates dst topic)
"$BUILD_DIR/rosenet_rx" "$DST_TOPIC" $PORT 1024 64 >/dev/null 2>&1 &
PID_RX=$!

pass "Liaison network started"

# Test message flow
if "$BUILD_DIR/test_net_sub" "$DST_TOPIC" 5 >/dev/null 2>&1; then
    pass "Message received through liaison"
else
    fail "No message received"
fi

echo -e "\n${GREEN}Network tests passed!${NC}"
exit 0
