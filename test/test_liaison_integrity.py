#!/usr/bin/env python3
"""Actual Quicksand source/sink, serial PTY and IPv4 loopback integrity tests."""
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import zlib

BUILD = Path(os.environ.get("BUILD_DIR", "build")).resolve()
BASE = f"liaison_integrity_{os.getpid()}"
ENV = dict(os.environ, ROSE_BASE=BASE)
PROCS = []
LOGS = []

def launch(exe, *args):
    log = tempfile.TemporaryFile()
    p = subprocess.Popen([str(BUILD / exe), *map(str, args)], env=ENV,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
    PROCS.append(p)
    LOGS.append(log)
    return p

def line(p, timeout=5):
    if not select.select([p.stdout], [], [], timeout)[0]:
        raise AssertionError(f"timeout waiting for {p.args}")
    value = p.stdout.readline().strip()
    assert value, (p.args, p.poll())
    return value

def fixture(mode, topic, size=131072):
    p = launch("test_liaison_fixture", mode, topic, size)
    assert line(p) == b"READY"
    return p

def action(p, n, seed):
    p.stdin.write(f"{n} {seed}\n".encode())
    p.stdin.flush()
    assert line(p) == b"OK"

def stop(p, expected=0):
    if p.poll() is None:
        p.send_signal(signal.SIGINT)
    assert p.wait(timeout=3) == expected, (p.args, p.returncode)

def done(p):
    p.stdin.close()
    assert p.wait(timeout=3) == 0

def port(kind=socket.SOCK_DGRAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    value = s.getsockname()[1]
    s.close()
    return value

def data(n, seed):
    return bytes((i + seed) & 255 for i in range(n))

def frame(payload):
    raw = b"\1" + struct.pack("<H", len(payload)) + payload
    raw += struct.pack("<I", zlib.crc32(raw))
    return b"~" + raw.replace(b"}", b"}\x5d").replace(b"~", b"}\x5e") + b"~"

def read_serial(fd, expected, timeout=3):
    result = bytearray()
    end = time.monotonic() + timeout
    while len(result) < len(expected) and time.monotonic() < end:
        if select.select([fd], [], [], max(0, end - time.monotonic()))[0]:
            result.extend(os.read(fd, len(expected) - len(result)))
    assert bytes(result) == expected, (len(result), len(expected))

def no_serial(fd):
    assert not select.select([fd], [], [], .05)[0]

try:
    # Invalid configurations must fail before allocating shared memory/opening I/O.
    for exe, args in [
        ("roseserial", ["/dev/null", "r", "t", 115200, 65536, 2]),
        ("roseserial", ["/dev/null", "r", "t", 115200, -1, 2]),
        ("roseserial", ["/dev/null", "r", "t", 115200, 64, 2]),
        ("rosenet_rx", ["--topic", "r", "--port", 12345, "--message_size", "9223372036854775807"]),
        ("rosenet_rx", ["--topic", "r", "--port", 12345, "--message_rate", "bad"]),
        ("rosenet_rx", ["--topic", "r", "--port", 12345, "--message_size", 1048576, "--message_rate", 512]),
        ("rosenet_tx", ["--topic", "r", "--host", "127.0.0.1", "--port", 12345, "--message_size", -1]),
    ]:
        p = launch(exe, *args)
        assert p.wait(timeout=3) == 1

    core = launch("rosecore")
    time.sleep(.15)
    source = fixture("pub", "source")

    udp_port = port()
    rx = launch("rosenet_rx", "--topic", "udp_rx", "--port", udp_port,
                "--udp", "--message_size", 64, "--message_rate", 16)
    sink = fixture("sub", "udp_rx")
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    time.sleep(.05)
    udp.sendto(data(65, 200), ("127.0.0.1", udp_port))
    udp.sendto(data(64, 1), ("127.0.0.1", udp_port))
    action(sink, 64, 1)
    udp.sendto(data(1, 9), ("127.0.0.1", udp_port))
    action(sink, 1, 9)
    done(sink); stop(rx)

    # TX consumes oversize source samples; the next valid sample must get through.
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.1", 0)); receiver.settimeout(2)
    tx = launch("rosenet_tx", "--topic", "source", "--host", "127.0.0.1",
                "--port", receiver.getsockname()[1], "--udp", "--message_size", 65536)
    time.sleep(.1)
    action(source, 65508, 3)
    time.sleep(.03)
    action(source, 65507, 4)
    assert receiver.recv(65536) == data(65507, 4)
    action(source, 131072, 6)
    time.sleep(.03)
    action(source, 17, 7)
    assert receiver.recv(65536) == data(17, 7)
    receiver.settimeout(.05)
    try:
        receiver.recv(65536)
        raise AssertionError("unexpected extra UDP datagram")
    except socket.timeout:
        pass
    stop(tx); receiver.close()

    # Broadcast without SO_BROADCAST deterministically rejects sendto; no
    # successful-send count is allowed and the failure must reach exit status.
    failed_tx = launch("rosenet_tx", "--topic", "source", "--host", "255.255.255.255",
                       "--port", 12345, "--udp")
    time.sleep(.1)
    action(source, 17, 55)
    assert failed_tx.wait(timeout=2) == 1
    failed_log = LOGS[PROCS.index(failed_tx)]
    failed_log.seek(0)
    assert b"sent=0" in failed_log.read()

    # Both serial directions use distinct messages and exact byte assertions.
    master, slave = os.openpty()
    serial = launch("roseserial", os.ttyname(slave), "serial_rx", "source", 115200, 64, 16)
    sink = fixture("sub", "serial_rx")
    time.sleep(.1)
    # A previously published sample may legitimately be read at initial attach.
    if select.select([master], [], [], .05)[0]:
        os.read(master, 65536)
    encoded = frame(data(64, 124))
    # Bytewise header and escaped data followed by delayed payload: state survives.
    for byte in encoded[:6]:
        os.write(master, bytes([byte])); time.sleep(.003)
    time.sleep(.08)
    os.write(master, encoded[6:])
    action(sink, 64, 124)
    # CRC damage, truncated payload, oversize, then a good frame; no prefix escapes.
    corrupt = bytearray(frame(data(16, 2))); corrupt[6] ^= 1
    os.write(master, corrupt)
    os.write(master, frame(data(32, 3))[:-7])
    os.write(master, frame(data(65, 5)))
    os.write(master, frame(data(12, 7)))
    action(sink, 12, 7)
    # A stale fragment is discarded until the next explicit boundary.
    stale = frame(data(24, 9))
    os.write(master, stale[:8]); time.sleep(1.1)
    os.write(master, stale[8:] + frame(data(20, 11)))
    action(sink, 20, 11)
    action(source, 65, 19); time.sleep(.03)
    action(source, 64, 125)
    read_serial(master, frame(data(64, 125)))
    no_serial(master)
    done(sink); stop(serial)
    os.close(master); os.close(slave)

    # Force partial nonblocking PTY writes beyond the terminal output queue.
    master, slave = os.openpty()
    serial = launch("roseserial", os.ttyname(slave), "large_serial", "source", 115200, 65535, 2)
    time.sleep(.1)
    if select.select([master], [], [], .05)[0]:
        os.read(master, 65536)
    action(source, 65536, 1); time.sleep(.03)
    action(source, 65535, 124)
    time.sleep(.05)  # Keep the finite PTY queue full before draining it.
    read_serial(master, frame(data(65535, 124)))
    stop(serial)
    os.close(master); os.close(slave)

    # TCP RX receives header/payload fragments and reports partial-frame timeout.
    tcp_port = port(socket.SOCK_STREAM)
    rx = launch("rosenet_rx", "tcp_rx", tcp_port, 64, 16)
    sink = fixture("sub", "tcp_rx")
    peer = socket.create_connection(("127.0.0.1", tcp_port), timeout=2)
    payload = data(64, 33)
    wire = struct.pack("<I", len(payload)) + payload
    for byte in wire:
        peer.sendall(bytes([byte])); time.sleep(.001)
    action(sink, 64, 33)
    peer.sendall(b"\x10\x00")
    assert rx.wait(timeout=2) == 1
    done(sink); peer.close()

    done(source); stop(core)
    print("liaison real PTY/UDP/TCP payload, corruption, truncation, limits and recovery: PASS")
finally:
    for p in reversed(PROCS):
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=2)
            except subprocess.TimeoutExpired: p.kill(); p.wait(timeout=2)
    for p, log in zip(PROCS, LOGS):
        log.seek(0)
        text = log.read().decode(errors="replace")
        if "AddressSanitizer" in text or "runtime error:" in text:
            raise AssertionError(text)
        if p.returncode not in (0, 1):
            print(p.args, p.returncode, text)
        log.close()
    # Only resources from this invocation; never broad shared-memory cleanup.
    for path in Path("/dev/shm").glob(BASE + "%*"):
        path.unlink(missing_ok=True)
