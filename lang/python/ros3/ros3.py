# ros3/ros3.py
# -------------
# High-level Python API for ROS3 (Rose) publisher/subscriber middleware.
#
# Example:
#     import ros3 as rose
#
#     node = rose.Node("my_node")
#     pub = node.publisher("chatter", message_size=4096, rate=100)
#     sub = node.subscriber("chatter")
#     rate = rose.Rate(10)  # 10 Hz
#
#     while node.ok():
#         pub.publish(["Hello", 42])
#         for msg in sub:
#             print(msg)
#         rate.sleep()

from __future__ import annotations
from typing import Any, Iterator
from threading import RLock
from . import _ros3 as _c

import mirage  # Required dependency for message encoding/decoding

log_info = _c.ROSE_LOG_INFO
log_warn = _c.ROSE_LOG_WARN
log_error = _c.ROSE_LOG_ERROR
log_fatal = _c.ROSE_LOG_FATAL


def _coerce_log_message(message: str | bytes | bytearray | memoryview) -> Any:
    if isinstance(message, str):
        return message.encode("utf-8")
    return message


# =============================================================================
# Core Classes
# =============================================================================

class Node:
    """A Rose node that can create publishers and subscribers.

    Native calls are serialized across nodes for safe C library access. Publishers
    and subscribers retain the native node until they are released. Explicit
    shutdown closes all endpoints; subsequent operations raise RuntimeError.

    Example:
        import ros3 as rose

        node = rose.Node("my_node")
        pub = node.publisher("topic", message_size=4096, rate=100)
        sub = node.subscriber("topic")

        while node.ok():
            pub.publish(["Hello", 42])
            for msg in sub:
                print(msg)
            rose.sleep(0.1)
    """

    def __init__(self, name: str, *, prefix: str = ""):
        """Create a new ROS3 node.

        Args:
            name: One-segment name, unique within this process and namespace
            prefix: Optional namespace for identity and relative topics
        """
        self._capsule, self._name = _c.create_node(name, prefix=prefix)

    @property
    def name(self) -> str:
        """The registry name: namespace/name-PID, or name-PID at root."""
        return self._name

    def ok(self) -> bool:
        """Check if the node should continue running.

        Call this regularly in your main loop. It also sends heartbeats
        to rosecore (rate-limited internally).
        """
        return _c.ok(self._capsule)

    def log(self, level: int, message: str | bytes | bytearray | memoryview) -> None:
        """Log a message through this node."""
        _c.log(self._capsule, level, _coerce_log_message(message))

    def info(self, message: str | bytes | bytearray | memoryview) -> None:
        """Log an informational message through this node."""
        self.log(log_info, message)

    def warn(self, message: str | bytes | bytearray | memoryview) -> None:
        """Log a warning message through this node."""
        self.log(log_warn, message)

    def error(self, message: str | bytes | bytearray | memoryview) -> None:
        """Log an error message through this node."""
        self.log(log_error, message)

    def fatal(self, message: str | bytes | bytearray | memoryview) -> None:
        """Log a fatal message through this node."""
        self.log(log_fatal, message)

    def shutdown(self) -> None:
        """Close the node and its endpoints. Repeated calls are harmless."""
        _c.shutdown(self._capsule)

    close = shutdown

    def publisher(self, topic: str, *, message_size: int,
                  rate: int) -> Publisher:
        """Create a publisher for a topic.

        Args:
            topic: Topic name (e.g., "sensor/imu", "cmd_vel")
            message_size: Maximum message size in bytes (required)
            rate: Maximum publish rate in Hz (required)
        """
        return Publisher(self._capsule, topic, message_size, rate)

    def subscriber(self, topic: str, *, latest_only: bool = False) -> Subscriber:
        """Create a subscriber for a topic.

        New subscribers skip any buffered messages and start at the latest
        position in the ring.

        Args:
            topic: Topic name to subscribe to
            latest_only: If True, only receive the most recent message
        """
        return Subscriber(self._capsule, topic, latest_only)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.shutdown()


class Publisher:
    """Publishes messages to a topic.

    Messages are automatically serialized using Mirage (WXF format).

    Example:
        pub = node.publisher("chatter", message_size=4096, rate=100)
        pub.publish(["Hello", 42, 3.14])
        pub.publish({"x": 1.0, "y": 2.0})
    """

    def __init__(self, node_capsule, topic: str, message_size: int, rate: int):
        self._topic = topic
        self._capsule = _c.create_pub(node_capsule, topic, message_size, rate)

    @property
    def topic(self) -> str:
        """The topic name."""
        return self._topic

    def publish(self, message: Any) -> None:
        """Publish a message (automatically encoded with Mirage)."""
        _c.publish(self._capsule, mirage.encode(message))

    def publish_raw(self, data: bytes) -> None:
        """Publish pre-encoded bytes directly."""
        _c.publish(self._capsule, data)


class Subscriber:
    """Receives messages from a topic.

    Messages are automatically deserialized using Mirage.

    Example:
        sub = node.subscriber("chatter")

        # Polling style
        msg = sub.read()
        if msg is not None:
            print(msg)

        # Iterator style (process all queued messages)
        for msg in sub:
            print(msg)
    """

    def __init__(self, node_capsule, topic: str, latest_only: bool):
        self._topic = topic
        self._read_lock = RLock()
        self._capsule, self._message_size = _c.create_sub(
            node_capsule, topic, 1 if latest_only else 0
        )
        self._buffer = bytearray(self._message_size)

    @property
    def topic(self) -> str:
        """The topic name."""
        return self._topic

    def _read_raw(self, *, latest: bool = False) -> bytes | None:
        # Include the copy in the critical section: native I/O releases the GIL.
        with self._read_lock:
            if latest:
                length = _c.readlast(self._capsule, self._buffer)
            else:
                result = _c.read(self._capsule, self._buffer)
                length = None if result is None else result[0]
            return None if length is None else bytes(self._buffer[:length])

    def read(self) -> Any | None:
        """Read one message, or None if unavailable.

        With latest_only=True, consume the newest queued message and skip older
        messages. Otherwise consume the next queued message in order.
        """
        data = self._read_raw()
        return None if data is None else mirage.decode(data)

    def read_latest(self) -> Any | None:
        """Consume the newest queued message, regardless of latest_only."""
        data = self._read_raw(latest=True)
        return None if data is None else mirage.decode(data)

    def read_raw(self) -> bytes | None:
        """Read bytes using the subscriber's latest_only policy."""
        return self._read_raw()

    def read_latest_raw(self) -> bytes | None:
        """Consume the newest queued message as bytes, skipping older messages."""
        return self._read_raw(latest=True)

    def __iter__(self) -> Iterator[Any]:
        """Poll queued messages using latest_only; stop when no data remains.

        A decoded None is yielded normally. Messages arriving during iteration
        may also be yielded.
        """
        while True:
            data = self._read_raw()
            if data is None:
                return
            yield mirage.decode(data)

    def __len__(self) -> int:
        """Number of messages waiting to be read."""
        val = _c.remaining(self._capsule)
        return 0 if val > (1 << 62) else val  # Handle native count underflow


class Rate:
    """Maintains a constant loop rate.  TODO(Alec: 2026, fix this)

    Example:
        import ros3 as rose

        rate = rose.Rate(10)  # 10 Hz
        while node.ok():
            do_work()
            rate.sleep()  # Sleeps just enough to maintain 10 Hz
    """

    def __init__(self, hz: float):
        """Create a rate controller.

        Args:
            hz: Target rate in Hz (e.g., 10 for 10 loops per second)
        """
        self.hz = hz

    def sleep(self) -> None:
        """Sleep to maintain the target rate."""
        _c.sleep_rate(self.hz)


# =============================================================================
# Simple Functions
# =============================================================================

def sleep(seconds: float) -> None:
    """Sleep for the specified duration.

    Args:
        seconds: Duration to sleep (e.g., 0.1 for 100ms)
    """
    _c.sleep(seconds)


def sleep_rate(hz: float) -> None:
    """Sleep to maintain a constant loop rate.

    Tracks time since last call and sleeps just enough to maintain the rate.
    Consider using Rate class for clearer code: `rate = rose.Rate(hz); rate.sleep()`

    Args:
        hz: Target rate in Hz
    """
    _c.sleep_rate(hz)


def now() -> int:
    """Get a monotonic timestamp (for measuring elapsed time)."""
    return _c.now()


def elapsed(start: int) -> float:
    """Get seconds elapsed since a timestamp from now().

    Example:
        import ros3 as rose

        start = rose.now()
        do_work()
        print(f"Took {rose.elapsed(start):.3f} seconds")
    """
    return _c.elapsed_seconds(start)


# =============================================================================
# Advanced Functions (less commonly needed)
# =============================================================================

def nanosleep(nanoseconds: float) -> None:
    """Sleep for nanoseconds (high-precision)."""
    _c.nanosleep(nanoseconds)


def seconds(stop: int, start: int) -> float:
    """Convert two timestamps to elapsed seconds."""
    return _c.seconds(stop, start)


def nanoseconds(stop: int, start: int) -> float:
    """Convert two timestamps to elapsed nanoseconds."""
    return _c.nanoseconds(stop, start)


def elapsed_nanoseconds(start: int) -> float:
    """Get nanoseconds elapsed since a timestamp."""
    return _c.elapsed_nanoseconds(start)


def timer_calibrate(duration: float = 1.0) -> None:
    """Calibrate the timer for accurate timing (call once at startup)."""
    _c.timer_calibrate(duration)


def topic_to_shm(topic: str) -> str:
    """Convert a topic name to its shared memory name (for debugging)."""
    return _c.topic_to_shm(topic)
