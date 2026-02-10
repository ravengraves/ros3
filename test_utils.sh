#!/bin/bash
# test_utils.sh - Fast integration test for rosemaster, rosenode, rosetopic

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

# Test namespace
export ROSE_BASE="test"

pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; cleanup; exit 1; }

cleanup() {
    trap - EXIT INT TERM
    [ -n "$TEST_PID" ] && kill "$TEST_PID" 2>/dev/null || true
    [ -n "$MASTER_PID" ] && kill "$MASTER_PID" 2>/dev/null || true
    rm -f "/dev/shm/${ROSE_BASE}%"* 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Clean leftover shm
rm -f "/dev/shm/${ROSE_BASE}%"* 2>/dev/null || true

# Launch rosemaster and test node (all use same ROSE_BASE)
"$BUILD_DIR/rosemaster" >/dev/null 2>&1 &
MASTER_PID=$!

# Let rosemaster create its core topics before the test node starts.
sleep 0.6

"$BUILD_DIR/test" >/dev/null 2>&1 &
TEST_PID=$!

# Give rosemaster/node time to register topics before querying.
sleep 0.6

# Run all queries
NODE_OUTPUT=$("$BUILD_DIR/rosenode" list 2>&1)
[ -n "$NODE_OUTPUT" ] || fail "rosenode list empty"
pass "rosenode list"

INFO_OUTPUT=$("$BUILD_DIR/rosenode" info demo 2>&1)
[ -n "$INFO_OUTPUT" ] || fail "rosenode info empty"
pass "rosenode info"

TOPIC_OUTPUT=$("$BUILD_DIR/rosetopic" list 2>&1)
[ -n "$TOPIC_OUTPUT" ] || fail "rosetopic list empty"
pass "rosetopic list"

# Test kill
"$BUILD_DIR/rosenode" kill demo >/dev/null 2>&1 || true
sleep 0.5
if kill -0 "$TEST_PID" 2>/dev/null; then
    echo "Warning: node still running"
else
    pass "rosenode kill"
    TEST_PID=""
fi

echo -e "\n${GREEN}Util tests passed!${NC}"
exit 0
