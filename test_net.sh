#!/bin/bash
# test_net.sh - Fast test for liaison network bridging
# Publisher -> /test/src -> rosenet_tx -> TCP -> rosenet_rx -> /test/dst -> Subscriber

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

export ROSE_BASE="test"

pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; cleanup; exit 1; }

SRC_TOPIC="/test/src"
DST_TOPIC="/test/dst"
PORT=19999

cleanup() {
    trap - EXIT INT TERM
    kill $PID_PUB $PID_LIAISON_OUT $PID_LIAISON_IN 2>/dev/null || true
    rm -f "/dev/shm/%test%src" "/dev/shm/%test%dst" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Clean leftover shared memory
rm -f "/dev/shm/%test%src" "/dev/shm/%test%dst" 2>/dev/null || true

# Launch all components with minimal delay
"$BUILD_DIR/test_net_pub" "$SRC_TOPIC" 50 >/dev/null 2>&1 &
PID_PUB=$!

"$BUILD_DIR/rosenet_rx" "$DST_TOPIC" $PORT 1024 64 >/dev/null 2>&1 &
PID_LIAISON_IN=$!

sleep 0.1

"$BUILD_DIR/rosenet_tx" "$SRC_TOPIC" 127.0.0.1 $PORT >/dev/null 2>&1 &
PID_LIAISON_OUT=$!

sleep 0.1
pass "Liaison network started"

# Test message flow
if "$BUILD_DIR/test_net_sub" "$DST_TOPIC" 5 >/dev/null 2>&1; then
    pass "Message received through liaison"
else
    fail "No message received"
fi

echo -e "\n${GREEN}Network tests passed!${NC}"
exit 0
