#!/usr/bin/env python3
"""Real systemd lifecycle checks; all workers/post-hooks are hardware-free."""
import os
from pathlib import Path
import signal
import subprocess as sp
import time

ROOT = Path(__file__).resolve().parents[2]
LAUNCH = ROOT / "tools/k2b-managed-session.sh"
POST = ROOT / "tests/k2b/session/post.sh"
WORKER = ROOT / "tests/k2b/session/worker.sh"
assert LAUNCH.is_file(), "FAIL: managed session entry point is missing"
assert os.geteuid() == 0, "run with sudo on the systemd board"


def ctl(*args):
    return sp.run(["systemctl", *args], text=True, capture_output=True)


def journal(unit):
    return sp.check_output(["journalctl", "-u", unit, "--no-pager", "-o", "cat"], text=True)


def until(predicate, message, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(message)


for index, mode in enumerate(("normal", "error", "term", "kill", "execfail", "interrupt", "parentkill", "duplicate")):
    unit = f"k2b-session-test-{os.getpid()}-{index}.service"
    worker_mode = "wait" if mode in ("interrupt", "parentkill", "duplicate") else mode
    command = ["/bin/sh", str(WORKER), worker_mode, "a b", "$literal", "%n"]
    env = dict(os.environ, PULSE_SERVER="unix:/tmp/k2b-test-pulse", PULSE_COOKIE="/tmp/k2b test cookie")
    process = sp.Popen(["/bin/sh", str(LAUNCH), unit, str(POST), *command], env=env,
                       stdout=sp.DEVNULL, stderr=sp.DEVNULL, start_new_session=True)
    try:
        if worker_mode == "wait":
            until(lambda: "WORKER_READY" in journal(unit), f"{mode}: worker did not start")
            if mode == "interrupt":
                process.send_signal(signal.SIGINT)
            elif mode == "parentkill":
                process.kill()
                process.wait(timeout=5)
                assert ctl("is-active", "--quiet", unit).returncode == 0
                ctl("stop", unit)
            else:
                duplicate = sp.run(["/bin/sh", str(LAUNCH), unit, str(POST), "/bin/true"],
                                   stdout=sp.DEVNULL, stderr=sp.PIPE, text=True, timeout=5)
                assert duplicate.returncode != 0, "duplicate session was accepted"
                assert ctl("is-active", "--quiet", unit).returncode == 0, "duplicate stopped original"
                ctl("stop", unit)
        result = process.wait(timeout=20)
        until(lambda: "POST_RESULT=" in journal(unit), f"{mode}: post-hook did not run")
        output = journal(unit)
        assert output.count("POST_RESULT=") == 1, output
        if mode == "normal":
            assert result == 0, result
            assert "POST_RESULT=success" in output, output
            assert "ARG=<a b>" in output and "ARG=<$literal>" in output, output
            assert "ARG=<%n>" in output, output
            assert "PULSE=<unix:/tmp/k2b-test-pulse>|</tmp/k2b test cookie>" in output, output
        if mode == "error":
            # Type=exec may report an immediate process exit as startup failure
            # (status 1); the journal still preserves the worker's exact code.
            assert result != 0, result
            assert "EXIT=exited STATUS=7" in output, output
        if mode == "kill":
            assert "EXIT=killed STATUS=KILL" in output, output
        print(f"PASS: {mode}", flush=True)
    finally:
        ctl("stop", unit)
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=20)

print("PASS: managed session lifecycle (8 cases)")
