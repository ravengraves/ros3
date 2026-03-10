#!/bin/bash
# test_cpp.sh - C++ publisher/subscriber integration test

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; cleanup; exit 1; }

cleanup() {
    trap - EXIT INT TERM
    [ -n "$SUB_PID" ] && kill "$SUB_PID" 2>/dev/null || true
    [ -n "$MASTER_PID" ] && kill "$MASTER_PID" 2>/dev/null || true
    rm -f "/dev/shm/${ROSE_BASE}%"* 2>/dev/null || true
}
trap cleanup EXIT INT TERM

export ROSE_BASE="test_cpp"

# Clean stale shared memory from previous runs.
rm -f "/dev/shm/${ROSE_BASE}%"* 2>/dev/null || true

"$BUILD_DIR/rosecore" >/dev/null 2>&1 &
MASTER_PID=$!

# Let rosecore create core topics before starting test nodes.
sleep 0.6

TOPIC="cpp_chatter"
COUNT=5
SUB_LOG="/tmp/ros3_cpp_subscriber.log"

"$BUILD_DIR/test_cpp_subscriber" "$TOPIC" "$COUNT" >"$SUB_LOG" 2>&1 &
SUB_PID=$!

sleep 0.2

CPP_OUTPUT="$($BUILD_DIR/test_cpp_publisher "$TOPIC" "$COUNT" 200 2>&1)" || {
    echo "$CPP_OUTPUT"
    fail "C++ publisher binary failed"
}

wait "$SUB_PID" || fail "C++ subscriber binary failed"
SUB_PID=""

RECEIVED_COUNT="$(grep -c '^Received:' "$SUB_LOG" || true)"
if [ "$RECEIVED_COUNT" -lt "$COUNT" ]; then
    cat "$SUB_LOG"
    fail "Expected $COUNT received messages, got $RECEIVED_COUNT"
fi

echo "$CPP_OUTPUT"
cat "$SUB_LOG"
pass "C++ publisher/subscriber"

echo -e "\n${GREEN}C++ test passed!${NC}"
exit 0
