#!/usr/bin/env python3
"""Real request/response rings: concurrent CLI queries and missing-core deadlines."""
import concurrent.futures
import os
from pathlib import Path
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
build = Path(os.environ.get('BUILD_DIR', root / 'build'))
base = f'query_test_{os.getpid()}'
env = dict(os.environ, ROSE_BASE=base, ROSE_QUERY_TIMEOUT_MS='500')
processes = []


def run(tool, *args):
    return subprocess.run([str(build / tool), *args], env=env,
                          capture_output=True, text=True, timeout=3)


try:
    with tempfile.TemporaryFile(mode='w+') as log:
        core = subprocess.Popen([str(build / 'rosecore')], env=env,
                                stdout=log, stderr=log)
        processes.append(core)
        deadline = time.monotonic() + 3
        while True:
            log.seek(0)
            if 'Ready.' in log.read():
                break
            assert core.poll() is None, 'rosecore exited during startup'
            assert time.monotonic() < deadline, 'rosecore startup timeout'
            time.sleep(.01)
        pub = subprocess.Popen([str(build / 'test_net_pub'), '/query_topic', '100'],
                               env=env, stdout=log, stderr=log)
        processes.append(pub)
        deadline = time.monotonic() + 3
        while True:
            reply = run('rosenode', 'info', '--pid', str(pub.pid))
            if reply.returncode == 0 and f'PID: {pub.pid}' in reply.stdout:
                break
            assert pub.poll() is None, 'publisher exited'
            assert time.monotonic() < deadline, reply.stderr
            time.sleep(.01)

        requests = [
            ('rosenode', 'list'),
            ('rosenode', 'info', '--pid', str(pub.pid)),
            ('rosenode', 'info', '--pid', str(os.getpid())),
            ('rosenode', 'info', 'test_net_pub'),
            ('rosetopic', 'list'),
            ('rosetopic', 'info', '/query_topic'),
        ] * 3
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            replies = list(pool.map(lambda args: run(*args), requests))
        for args, reply in zip(requests, replies):
            assert reply.returncode == 0, (args, reply.stdout, reply.stderr)
            if args[-1] == str(os.getpid()):
                assert 'No matching nodes found' in reply.stdout
            elif args[0] == 'rosetopic':
                assert '/query_topic' in reply.stdout, (args, reply.stdout)
            else:
                assert 'test_net_pub' in reply.stdout, (args, reply.stdout)

        core.terminate()
        core.wait(timeout=3)
        env['ROSE_QUERY_TIMEOUT_MS'] = '50'
        for args in [('rosenode', 'list'), ('rosenode', 'info', 'missing'),
                     ('rosenode', 'kill', 'missing'), ('rosetopic', 'list')]:
            start = time.monotonic()
            reply = run(*args)
            assert reply.returncode != 0 and 'timed out' in reply.stderr, (args, reply)
            assert time.monotonic() - start < 1
    print('query CLI checks passed: concurrent dumps/lookups and bounded timeouts')
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
    # Only this test's unique namespace; include rings created without a core.
    shm = Path('/dev/shm')
    if shm.exists():
        for path in shm.glob(base + '%*'):
            path.unlink(missing_ok=True)
