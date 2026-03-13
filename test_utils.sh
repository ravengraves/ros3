#!/bin/bash
# test_utils.sh - Fast integration test for rosecore, rosenode, rosetopic

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

# Test namespace
export ROSE_BASE="test"

pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; cleanup; exit 1; }

wait_for_nonempty_output() {
    local __outvar="$1"
    shift

    local output=""
    local attempt
    for attempt in $(seq 1 50); do
        if output=$("$@" 2>&1) && [ -n "$output" ]; then
            printf -v "$__outvar" '%s' "$output"
            return 0
        fi
        sleep 0.1
    done

    printf -v "$__outvar" '%s' "$output"
    return 1
}

expect_echo_fail() {
    local topic="$1"

    "$BUILD_DIR/rosetopic" echo "$topic" >/dev/null 2>&1 &
    local pid=$!
    sleep 0.3

    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi

    wait "$pid"
    [ $? -ne 0 ]
}

remove_topic() {
    "$BUILD_DIR/rosetopic" rm "$1" >/dev/null 2>&1 || true
}

cleanup_topics() {
    remove_topic /chatter
    remove_topic /rosecore
    remove_topic /rosecore_dump
    remove_topic /rosecore_lookup
    remove_topic /rosecore_heartbeat
}

cleanup() {
    trap - EXIT INT TERM
    [ -n "$TEST_PID" ] && kill "$TEST_PID" 2>/dev/null || true
    [ -n "$SECOND_MASTER_PID" ] && kill "$SECOND_MASTER_PID" 2>/dev/null || true
    [ -n "$MASTER_PID" ] && kill "$MASTER_PID" 2>/dev/null || true
    cleanup_topics
}
trap cleanup EXIT INT TERM

# Clean leftover topics from prior runs.
cleanup_topics

# Launch rosecore and test node (all use same ROSE_BASE)
"$BUILD_DIR/rosecore" >/dev/null 2>&1 &
MASTER_PID=$!

# Let rosecore create its core topics before the test node starts.
sleep 0.6

"$BUILD_DIR/rosecore" >/dev/null 2>&1 &
SECOND_MASTER_PID=$!
sleep 0.5
if kill -0 "$SECOND_MASTER_PID" 2>/dev/null; then
    fail "second rosecore did not abort"
fi
if wait "$SECOND_MASTER_PID"; then
    fail "second rosecore unexpectedly succeeded"
fi
SECOND_MASTER_PID=""
pass "rosecore duplicate startup rejection"

"$BUILD_DIR/test" >/dev/null 2>&1 &
TEST_PID=$!

# Run all queries once rosecore/node registration is visible.
wait_for_nonempty_output NODE_OUTPUT "$BUILD_DIR/rosenode" list \
    || fail "rosenode list empty"
pass "rosenode list"

wait_for_nonempty_output INFO_OUTPUT "$BUILD_DIR/rosenode" info demo \
    || fail "rosenode info empty"
pass "rosenode info"

wait_for_nonempty_output TOPIC_OUTPUT "$BUILD_DIR/rosetopic" list \
    || fail "rosetopic list empty"
pass "rosetopic list"

"$BUILD_DIR/rosetopic" rm /chatter >/dev/null 2>&1 || fail "rosetopic rm /chatter"
expect_echo_fail /chatter || fail "rosetopic rm left /chatter connectable"
pass "rosetopic rm"

# Test kill
"$BUILD_DIR/rosenode" kill demo >/dev/null 2>&1 || true
sleep 0.5
if kill -0 "$TEST_PID" 2>/dev/null; then
    echo "Warning: node still running"
else
    pass "rosenode kill"
    TEST_PID=""
fi

kill "$MASTER_PID" 2>/dev/null || true
wait "$MASTER_PID" 2>/dev/null || true
MASTER_PID=""

expect_echo_fail /rosecore || fail "rosecore topic still connectable after exit"
expect_echo_fail /rosecore_dump || fail "rosecore dump topic still connectable after exit"
expect_echo_fail /rosecore_lookup || fail "rosecore lookup topic still connectable after exit"
expect_echo_fail /rosecore_heartbeat || fail "rosecore heartbeat topic still connectable after exit"
pass "rosecore core topic cleanup"

echo -e "\n${GREEN}Util tests passed!${NC}"
exit 0
