"""Real transport and discovery agree on names, remaps, identity, and limits."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

build = Path(os.environ.get("BUILD_DIR", "build")).resolve()
base = f"names_{os.getpid()}"
env = dict(os.environ, ROSE_BASE=base, ROSE_QUERY_TIMEOUT_MS="500")
processes = []


def query(tool, *args):
    result = subprocess.run([str(build / tool), *args], env=env,
                            capture_output=True, text=True, timeout=3)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    assert "AddressSanitizer" not in result.stderr and "runtime error:" not in result.stderr
    return result.stdout


def wait_for(log, text, process):
    deadline = time.monotonic() + 5
    while True:
        log.seek(0)
        output = log.read()
        assert process.poll() is None, output
        if text in output:
            return output
        assert time.monotonic() < deadline, output
        time.sleep(.01)


try:
    with tempfile.TemporaryFile(mode="w+") as core_log, tempfile.TemporaryFile(mode="w+") as node_log:
        core = subprocess.Popen([str(build / "rosecore")], env=env,
                                stdout=core_log, stderr=core_log)
        processes.append(core)
        wait_for(core_log, "Ready.", core)
        fixture = subprocess.Popen([str(build / "test_name_resolution_transport")], env=env,
                                   stdin=subprocess.PIPE, stdout=node_log, stderr=node_log, text=True)
        processes.append(fixture)
        output = wait_for(node_log, "READY\n", fixture)
        one, two, long_name, long_topic = output.split("READY\n", 1)[1].splitlines()[:4]
        assert one == f"one/same-{fixture.pid}" and two == f"two/same-{fixture.pid}"
        deadline = time.monotonic() + 3
        while True:
            listing = query("rosenode", "list")
            if all(name in listing for name in [one, two, long_name]):
                break
            assert time.monotonic() < deadline, listing
            time.sleep(.01)
        by_pid = query("rosenode", "info", "--pid", str(fixture.pid))
        assert all(name in by_pid for name in [one, two, long_name]), by_pid
        assert one in query("rosenode", "info", "one/same")
        assert two in query("rosenode", "info", "two/same")
        mapped = query("rosetopic", "info", "/mapped")
        assert one in mapped and two in mapped, mapped
        assert long_name in query("rosetopic", "info", long_topic)
        topics = query("rosetopic", "list")
        assert "/mapped" in topics and long_topic in topics, topics
        fixture.communicate("\n", timeout=3)
        assert fixture.returncode == 0
        node_log.seek(0)
        output = node_log.read()
        assert "AddressSanitizer" not in output and "runtime error:" not in output, output
        core.terminate(); core.wait(timeout=3)
        core_log.seek(0)
        output = core_log.read()
        assert "AddressSanitizer" not in output and "runtime error:" not in output, output
    print("name resolution transport/CLI passed: remaps, argc130, frozen base, namespace identity, PID lookup, 245-byte channels")
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate(); process.wait(timeout=3)
    for path in Path("/dev/shm").glob(base + "%*"):
        path.unlink(missing_ok=True)
