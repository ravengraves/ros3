#!/usr/bin/env python3
"""
Rose Python Bindings Test Suite

Tests the core pub/sub functionality of the Rose middleware.
"""

import sys
import os
import ros3 as rose
import mirage


def unique_topic(base: str) -> str:
    """Generate a unique topic name to avoid stale shared memory."""
    return f"{base}_{os.getpid()}"


def test_timing():
    """Test timing utilities."""
    print("Testing timing utilities...")

    t1 = rose.now()
    assert t1 > 0, "now() should return positive timestamp"

    rose.sleep(0.01)  # 10ms

    t2 = rose.now()
    assert t2 > t1, "time should advance"

    elapsed_time = rose.elapsed(t1)
    assert elapsed_time >= 0.01, f"elapsed should be >= 10ms, got {elapsed_time}s"
    assert elapsed_time < 1.0, f"elapsed should be < 1s, got {elapsed_time}s"

    duration = rose.seconds(t2, t1)
    assert duration >= 0.01, f"duration should be >= 10ms, got {duration}s"

    ns = rose.nanoseconds(t2, t1)
    assert ns >= 10_000_000, f"should be >= 10ms in ns, got {ns}ns"

    print(f"  now(): {t1}")
    print(f"  elapsed(): {elapsed_time:.4f}s")
    print("  PASSED")


def test_topic_naming():
    """Test topic to shared memory name conversion."""
    print("Testing topic naming...")

    shm = rose.topic_to_shm("chatter")
    assert "chatter" in shm, f"shm name should contain topic: {shm}"

    shm = rose.topic_to_shm("/my/nested/topic")
    assert "%" in shm, f"shm name should have % separators: {shm}"

    print(f"  'chatter' -> '{shm}'")
    print("  PASSED")


def test_node_creation():
    """Test node creation and shutdown."""
    print("Testing node creation...")

    node = rose.Node("test_node")
    assert node.name.startswith("test_node"), f"unexpected name: {node.name}"
    assert "-" in node.name, f"should have PID suffix: {node.name}"
    assert node.ok(), "node.ok() should return True"

    node.shutdown()
    print(f"  Created node: {node.name}")
    print("  PASSED")


def test_pubsub_basic():
    """Test basic pub/sub communication."""
    print("Testing basic pub/sub...")

    node = rose.Node("pubsub_test")
    pub = node.publisher("test_topic", message_size=4096, rate=100)
    sub = node.subscriber("test_topic")

    test_data = ["Hello", 42, 3.14159]
    pub.publish(test_data)
    rose.sleep(0.001)

    msg = sub.read()
    assert msg is not None, "should receive message"
    assert msg[0] == "Hello", f"first element should be 'Hello': {msg}"
    assert msg[1] == 42, f"second element should be 42: {msg}"
    assert abs(msg[2] - 3.14159) < 0.0001, f"third element should be pi: {msg}"

    node.shutdown()
    print(f"  Sent: {test_data}")
    print(f"  Received: {msg}")
    print("  PASSED")


def test_pubsub_types():
    """Test various data types through pub/sub."""
    print("Testing data types...")

    node = rose.Node("types_test")
    pub = node.publisher("types", message_size=8192, rate=100)
    sub = node.subscriber("types")

    test_cases = [
        ("integer", 42),
        ("negative", -1000),
        ("float", 3.14159265358979),
        ("string", "Hello, World!"),
        ("list", [1, 2, 3, 4, 5]),
        ("nested_list", [[1, 2], [3, 4]]),
        ("dict", {"x": 1.0, "y": 2.0, "z": 3.0}),
        ("mixed", ["data", 42, 3.14, {"key": "value"}]),
        ("bool_true", True),
        ("bool_false", False),
        ("none", None),
        ("expr_fn", mirage.Expr("Plus", 1, 2)),
    ]

    for name, data in test_cases:
        pub.publish(data)
        rose.sleep(0.001)

        # Special case for None: use read_raw() to distinguish between
        # "no data available" and "received a None value"
        if data is None:
            raw = sub.read_raw()
            if raw is None:
                print(f"  FAIL: {name} - no message received")
                continue
            msg = mirage.decode(raw)
        else:
            msg = sub.read()
            if msg is None:
                print(f"  FAIL: {name} - no message received")
                continue

        if isinstance(data, float):
            assert abs(msg - data) < 1e-10, f"FAIL: {name}: {msg} != {data}"
        elif isinstance(data, mirage.Expr):
            # Allow decode returning Expr or list-equivalent
            same = msg == data or (isinstance(msg, list) and msg == list(data))
            assert same, f"FAIL: {name}: {msg} != {data}"
        else:
            assert msg == data, f"FAIL: {name}: {msg} != {data}"

        print(f"  {name}: OK")

    node.shutdown()
    print("  PASSED")


def test_pubsub_iterator():
    """Test subscriber as iterator."""
    print("Testing subscriber iterator...")

    topic = unique_topic("iter")
    node = rose.Node("iter_test")
    pub = node.publisher(topic, message_size=1024, rate=100)
    sub = node.subscriber(topic)

    for i in range(5):
        pub.publish(["Hello", i])

    rose.sleep(0.01)

    messages = list(sub)
    assert len(messages) == 5, f"should receive 5 messages, got {len(messages)}"

    for i, msg in enumerate(messages):
        assert msg == ["Hello", i], f"message {i} mismatch: {msg}"

    node.shutdown()
    print(f"  Sent and received {len(messages)} messages")
    print("  PASSED")


def test_pubsub_remaining():
    """Test remaining message count."""
    print("Testing remaining message count...")

    topic = unique_topic("remaining")
    node = rose.Node("remaining_test")
    pub = node.publisher(topic, message_size=1024, rate=100)
    sub = node.subscriber(topic)

    for i in range(3):
        pub.publish(i)

    rose.sleep(0.02)

    received = 0
    while True:
        msg = sub.read()
        if msg is None:
            break
        received += 1

    assert received == 3, f"should receive 3 messages, got {received}"

    node.shutdown()
    print(f"  Published 3, received {received}")
    print("  PASSED")


def test_pubsub_latest_only():
    """Test latest_only subscriber mode."""
    print("Testing latest_only mode...")

    node = rose.Node("latest_test")
    pub = node.publisher("latest", message_size=1024, rate=100)
    sub = node.subscriber("latest", latest_only=True)

    for i in range(5):
        pub.publish(["Hello", i])

    rose.sleep(0.01)

    msg = sub.read()
    assert msg == ["Hello", 4], f"should get latest message (4), got {msg}"
    assert sub.read() is None
    print(f"  Got latest: {msg}")

    node.shutdown()
    print("  PASSED")


def test_sleep_rate():
    """Test rate-maintaining sleep function."""
    print("Testing sleep_rate function...")

    iterations = 5
    target_hz = 100.0
    target_period = 1.0 / target_hz

    start = rose.now()

    for _ in range(iterations):
        rose.sleep_rate(target_hz)

    elapsed = rose.elapsed(start)
    expected = iterations * target_period

    assert elapsed >= expected * 0.5, f"too fast: {elapsed}s < {expected * 0.5}s"
    assert elapsed < expected * 2.0, f"too slow: {elapsed}s > {expected * 2.0}s"

    print(f"  {iterations} iterations at {target_hz}Hz took {elapsed:.4f}s")
    print("  PASSED")


def test_rate_class():
    """Test Rate class for rate-controlled loops."""
    print("Testing Rate class...")

    iterations = 5
    target_hz = 100.0
    target_period = 1.0 / target_hz

    rate = rose.Rate(target_hz)
    start = rose.now()

    for _ in range(iterations):
        rate.sleep()

    elapsed = rose.elapsed(start)
    expected = iterations * target_period

    assert elapsed >= expected * 0.5, f"too fast: {elapsed}s < {expected * 0.5}s"
    assert elapsed < expected * 2.0, f"too slow: {elapsed}s > {expected * 2.0}s"

    print(f"  Rate({target_hz}).sleep() x {iterations} took {elapsed:.4f}s")
    print("  PASSED")


def test_context_manager():
    """Test node as context manager."""
    print("Testing context manager...")

    with rose.Node("context_test") as node:
        pub = node.publisher("context", message_size=1024, rate=100)
        pub.publish(["Hello", 0])
        assert node.ok()

    print("  PASSED")


def test_raw_publish():
    """Test raw byte publishing."""
    print("Testing raw publish...")

    topic = unique_topic("raw")
    node = rose.Node("raw_test")
    pub = node.publisher(topic, message_size=1024, rate=100)
    sub = node.subscriber(topic)

    # Encode manually with mirage
    data = mirage.encode(["Hello", 123])
    pub.publish_raw(data)

    rose.sleep(0.001)

    raw = sub.read_raw()
    assert raw == data, "raw data should match"

    pub.publish_raw(data)
    rose.sleep(0.001)
    msg = sub.read()
    assert msg == ["Hello", 123], f"decoded message mismatch: {msg}"

    node.shutdown()
    print("  PASSED")


def main():
    print("=" * 60)
    print("Rose Python Bindings Test Suite")
    print("=" * 60)
    print()

    tests = [
        test_timing,
        test_topic_naming,
        test_node_creation,
        test_pubsub_basic,
        test_pubsub_types,
        test_pubsub_iterator,
        test_pubsub_remaining,
        test_pubsub_latest_only,
        test_sleep_rate,
        test_rate_class,
        test_context_manager,
        test_raw_publish,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            test()
            passed += 1
        except Exception as e:
            print(f"  FAILED: {e}")
            failed += 1
        print()

    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
