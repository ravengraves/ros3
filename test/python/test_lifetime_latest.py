"""Focused real-transport regression checks; failures exit nonzero.

Run with the rebuilt ros3 package on PYTHONPATH and matching shared libraries on
LD_LIBRARY_PATH. No core is required. Only this process's ROSE_BASE is cleaned.
"""
import gc
import os
from pathlib import Path
import threading
import weakref

import mirage
import ros3 as rose
from ros3 import _ros3 as c

base = f"python_lifetime_{os.getpid()}"
os.environ["ROSE_BASE"] = base


def closed(call):
    try:
        call()
    except RuntimeError as exc:
        assert "closed" in str(exc), str(exc)
    else:
        raise AssertionError("closed handle accepted")


def semantics():
    with rose.Node("semantics") as node:
        pub = node.publisher("queue", message_size=1024, rate=1000)
        fifo = node.subscriber("queue")
        latest = node.subscriber("queue", latest_only=True)
        for value in range(3):
            pub.publish(value)
        assert rose.ROSE_LOG_INFO == rose.log_info
        node.info("lifetime regression")
        node.warn("lifetime regression")
        assert fifo.read() == 0
        assert latest.read() == 2
        assert latest.read_raw() is None
        assert fifo.read_latest() == 2
        assert fifo.read() is None
        for value in range(3, 6):
            pub.publish(value)
        assert mirage.decode(latest.read_raw()) == 5
        assert list(fifo) == [3, 4, 5]
        for value in range(6, 9):
            pub.publish(value)
        assert list(latest) == [8]
        assert mirage.decode(fifo.read_latest_raw()) == 8
        pub.publish(None)
        pub.publish("after-none")
        assert list(fifo) == [None, "after-none"]
        node.shutdown()  # Context exit calls it again.
    node.close()
    for call in [node.ok, lambda: node.publisher("x", message_size=64, rate=1),
                 lambda: node.subscriber("queue"), lambda: pub.publish(1),
                 fifo.read, fifo.read_raw, fifo.read_latest,
                 lambda: len(fifo), lambda: list(fifo), lambda: c.spin_once(node._capsule)]:
        closed(call)


def retained_parent():
    node = rose.Node("retained")
    pub = node.publisher("retained", message_size=128, rate=100)
    sub = node.subscriber("retained")
    ref = weakref.ref(node)
    del node
    gc.collect()
    assert ref() is None
    pub.publish("still alive")
    assert sub.read() == "still alive"
    del sub, pub
    gc.collect()  # Last endpoint releases node and shuts down native state.
    # Registry identity can be reused after automatic shutdown.
    with rose.Node("retained"):
        pass


def delete_aliases():
    with rose.Node("delete") as node, rose.Node("other") as other:
        pub = node.publisher("delete", message_size=128, rate=100)
        sub = node.subscriber("delete")
        alias = sub._capsule
        try:
            c.delete_sub(other._capsule, alias)
        except ValueError:
            pass
        else:
            raise AssertionError("cross-node delete accepted")
        c.delete_sub(node._capsule, alias)
        closed(sub.read)
        closed(lambda: c.read(alias, bytearray(128)))
    for call in [lambda: c.ok(object()), lambda: c.publish(alias, b"x")]:
        try:
            call()
        except TypeError:
            pass
        else:
            raise AssertionError("invalid capsule accepted")


def concurrent_io():
    node = rose.Node("concurrent")
    pub = node.publisher("concurrent", message_size=128, rate=10000)
    sub = node.subscriber("concurrent")
    errors = []
    start = threading.Barrier(5)
    progressed = [threading.Event() for _ in range(4)]

    def worker(operation, ready):
        start.wait(timeout=5)
        try:
            for _ in range(2000):
                operation()
                ready.set()
        except RuntimeError as exc:
            if "closed" not in str(exc):
                errors.append(exc)
        except BaseException as exc:
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(op, ready)) for op, ready in zip(
               [lambda: pub.publish(7), sub.read, sub.read, node.ok], progressed)]
    for thread in threads:
        thread.start()
    start.wait(timeout=5)
    for ready in progressed:
        assert ready.wait(timeout=5), "worker never entered native I/O"
    node.shutdown()
    for thread in threads:
        thread.join(timeout=10)
        assert not thread.is_alive(), "I/O/shutdown deadlocked"
    assert not errors, errors
    node.shutdown()


def concurrent_reads():
    with rose.Node("concurrent_reads") as node:
        pub = node.publisher("values", message_size=128, rate=1000)
        sub = node.subscriber("values")
        for value in range(200):
            pub.publish(value)
        results = []
        errors = []
        def reader():
            try:
                results.extend(sub)
            except BaseException as exc:
                errors.append(exc)
        threads = [threading.Thread(target=reader) for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)
            assert not thread.is_alive(), "concurrent reads deadlocked"
        assert not errors, errors
        assert sorted(results) == list(range(200)), results


try:
    semantics()
    retained_parent()
    delete_aliases()
    concurrent_reads()
    concurrent_io()
    print("Python lifetime/latest-only regressions passed")
finally:
    gc.collect()
    for path in Path("/dev/shm").glob(base + "%*"):
        path.unlink(missing_ok=True)
