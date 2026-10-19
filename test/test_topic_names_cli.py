"""Invalid names must fail before attaching, querying, or unlinking a topic."""
import os
from pathlib import Path
import subprocess


build = Path(os.environ.get("BUILD_DIR", "build")).resolve()


def rejected(tool, args, base):
    result = subprocess.run(
        [str(build / tool), *args],
        env=dict(os.environ, ROSE_BASE=base),
        capture_output=True,
        text=True,
        timeout=3,
    )
    assert result.returncode == 1, (tool, args, base, result.returncode, result.stderr)
    assert "AddressSanitizer" not in result.stderr, result.stderr
    assert "runtime error:" not in result.stderr, result.stderr


for tool, args in [("rosecore", []), ("rosenode", ["list"]), ("rosetopic", ["list"])]:
    for base in ["", "/", "a%b", "a,b", "//bad", "a/b", "b" * 226, "/" + "b" * 227 + "/"]:
        rejected(tool, args, base)

for command in ["echo", "hz", "rm", "info"]:
    for topic in ["", "a%b", "a,b", "a//b", "a/", "/", "t" * (246 - len("/routing_cli%"))]:
        rejected("rosetopic", [command, topic], "routing_cli")
    for base in ["b" * 226, "/" + "b" * 227 + "/"]:
        rejected("rosetopic", [command, "valid_topic"], base)

print("CLI canonical name and base-length rejection passed (60 subprocesses)")
