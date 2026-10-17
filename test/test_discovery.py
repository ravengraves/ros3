"""Real C/Python nodes keep their endpoints through late starts and core restarts."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

build = Path(os.environ.get("BUILD_DIR", "build")).resolve()
base = f"discovery_{os.getpid()}"
os.environ["ROSE_BASE"] = base
env = dict(os.environ, ROSE_QUERY_TIMEOUT_MS="500")
processes = []
logs = []
python_node = None


def spawn(program, *args, gate=None):
    log = tempfile.TemporaryFile(mode="w+")
    logs.append(log)
    command = [str(build / program), *args]
    if gate is not None:
        command = [sys.executable, "-c",
                   "import os,sys; print('ARMED', flush=True); os.read(int(sys.argv[1]),1); "
                   "os.close(int(sys.argv[1])); os.execv(sys.argv[2],sys.argv[2:])", str(gate), *command]
    process = subprocess.Popen(command, env=env, pass_fds=() if gate is None else (gate,),
                               stdin=subprocess.PIPE, stdout=log, stderr=log, text=True)
    processes.append(process)
    return process, log


def output(log):
    log.seek(0)
    return log.read()


def pump():
    if python_node:
        assert python_node.ok()
        python_pub.publish_raw(b"running")
        assert python_sub.read_raw() == b"running"
    time.sleep(.02)


def until(check, detail, timeout=5):
    deadline = time.monotonic() + timeout
    while not check():
        assert time.monotonic() < deadline, detail()
        pump()


def query(tool, *args):
    result = subprocess.run([str(build / tool), *args], env=env,
                            capture_output=True, text=True, timeout=2)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout


def discovered():
    listing = query("rosenode", "list")
    topics = query("rosetopic", "list")
    return all(name in listing for name in names) and all(topic in topics for topic in expected_topics)


try:
    if "--python" in sys.argv:
        import ros3
        python_node = ros3.Node("python_node", prefix="python")
        python_pub = python_node.publisher("data", message_size=64, rate=16)
        python_sub = python_node.subscriber("data")
    fixtures = [spawn("test_discovery", mode) for mode in ("ok", "spin")]
    names = [f"{mode}/c_node-{process.pid}" for mode, (process, _) in zip(("ok", "spin"), fixtures)]
    expected_topics = ["/ok/data", "/spin/data"]
    if python_node:
        names.append(python_node.name)
        expected_topics.append("/python/data")
    for process, log in fixtures:
        until(lambda: "READY" in output(log), lambda: output(log))
        assert process.poll() is None, output(log)
    # Traffic works before any core exists. Node startup does not wait for it.
    until(lambda: all(output(log).count("TICK") >= 8 for _, log in fixtures),
          lambda: [output(log) for _, log in fixtures])
    channels = [Path("/dev/shm") / f"{base}%{name}" for name in
                ("rosecore", "rosecore_heartbeat", "rosecore_request", "rosecore_dump", "rosecore_lookup")]
    inodes = None
    for cycle in range(3):
        if cycle == 0:
            reader, writer = os.pipe()
            try:
                candidates = [spawn("rosecore", gate=reader) for _ in range(2)]
                until(lambda: all("ARMED" in output(log) for _, log in candidates),
                      lambda: [output(log) for _, log in candidates])
                assert os.write(writer, b"xx") == 2
            finally:
                os.close(reader)
                os.close(writer)
            (core, core_log), (loser, loser_log) = sorted(candidates, key=lambda pair: pair[0].pid)
            until(lambda: loser.poll() is not None, lambda: output(loser_log))
            assert loser.returncode != 0 and "Another rosecore" in output(loser_log), output(loser_log)
        else:
            core, core_log = spawn("rosecore")
        until(lambda: "Ready." in output(core_log), lambda: output(core_log))
        current_inodes = [path.stat().st_ino for path in channels]
        if inodes is None:
            inodes = current_inodes
        assert current_inodes == inodes, "restart replaced a shared-memory ring"
        until(discovered, lambda: query("rosenode", "list"))
        for name in names:
            info = query("rosenode", "info", name)
            assert "data" in info, info
        for topic in expected_topics:
            info = query("rosetopic", "info", topic)
            relevant = [name for name in names if name.startswith(topic.split("/")[1] + "/")]
            for name in relevant:
                # Exactly once in each publisher/subscriber list after replay.
                assert info.count(name) == 2, info
        if cycle == 0:
            duplicate, duplicate_log = spawn("rosecore")
            until(lambda: duplicate.poll() is not None, lambda: output(duplicate_log))
            assert duplicate.returncode != 0 and "Another rosecore" in output(duplicate_log)
            core.terminate()
        elif cycle == 1:
            core.kill()
        else:
            break
        core.wait(timeout=3)
        assert [path.stat().st_ino for path in channels] == inodes
        ticks = [output(log).count("TICK") for _, log in fixtures]
        if cycle == 0:
            for process, _ in fixtures:
                process.stdin.write("a")
                process.stdin.flush()
            expected_topics.extend(["/ok/added", "/spin/added"])
        until(lambda: all(output(log).count("TICK") >= tick + 8
                          for (_, log), tick in zip(fixtures, ticks)),
              lambda: [output(log) for _, log in fixtures])
    # Centralized management works with the rebuilt registry too.
    for (process, _), name in zip(fixtures, names):
        assert "SIGINT" in query("rosenode", "kill", name)
        process.wait(timeout=3)
        assert process.returncode == 0
    core.terminate()
    core.wait(timeout=3)
    for log in logs:
        text = output(log)
        assert "AddressSanitizer" not in text and "runtime error:" not in text, text
    print("discovery integration passed: startup election, late core, persistent rings, graceful/crash restart, CLI and traffic")
except BaseException:
    for log in logs:
        print(output(log), file=sys.stderr)
    raise
finally:
    if python_node:
        python_node.shutdown()
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
    for log in logs:
        log.close()
    for path in Path("/dev/shm").glob(base + "%*"):
        path.unlink(missing_ok=True)
