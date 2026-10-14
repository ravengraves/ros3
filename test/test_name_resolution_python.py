"""Load a rebuilt native wrapper directly, avoiding unrelated Python deps.

Set ROS3_NATIVE_EXTENSION to its path, and put the matching shared libraries in
LD_LIBRARY_PATH. This deliberately does not exercise stale capsule lifetimes.
"""
import importlib.util
import os
from pathlib import Path

spec = importlib.util.spec_from_file_location("_ros3", os.environ["ROS3_NATIVE_EXTENSION"])
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)
base = f"python_names_{os.getpid()}"
bases = [base, base + "_other"]
nodes = []


def rejects(call):
    try:
        call()
    except (RuntimeError, ValueError):
        return
    raise AssertionError("invalid name accepted")


try:
    os.environ["ROSE_BASE"] = base
    one, name = c.create_node("same", prefix="one")
    nodes.append(one)
    assert name == f"one/same-{os.getpid()}"
    two, name = c.create_node("same", prefix="two")
    nodes.append(two)
    assert name == f"two/same-{os.getpid()}"
    os.environ["ROSE_BASE"] = bases[1]
    other, name = c.create_node("same", prefix="one")
    nodes.append(other)
    assert c.topic_to_shm("/shared") == f"/{bases[1]}%shared"
    os.environ["ROSE_BASE"] = "invalid/base"
    pub = c.create_pub(one, "shared", 128, 16)
    sub, _ = c.create_sub(two, "/one/shared", 0)
    c.publish(pub, b"naming")
    received = bytearray(128)
    size, _ = c.read(sub, received)
    assert received[:size] == b"naming"
    other_pub = c.create_pub(other, "shared", 128, 16)
    other_sub, _ = c.create_sub(other, "/one/shared", 0)
    c.publish(other_pub, b"isolated")
    size, _ = c.read(other_sub, received)
    assert received[:size] == b"isolated"
    assert c.read(sub, received) is None
    os.environ["ROSE_BASE"] = base
    for bad in ["a,b", "a%b", "a\x00b", "a//b"]:
        rejects(lambda: c.create_node(bad))
        rejects(lambda: c.create_node("bad", prefix=bad))
        rejects(lambda: c.create_pub(one, bad, 128, 16))
        rejects(lambda: c.topic_to_shm(bad))
    print("Python native naming passed: namespace identity, frozen bases, transport isolation, exact spans")
finally:
    for node in nodes:
        c.shutdown(node)
    for namespace in bases:
        for path in Path("/dev/shm").glob(namespace + "%*"):
            path.unlink(missing_ok=True)
