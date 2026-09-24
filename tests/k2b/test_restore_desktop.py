#!/usr/bin/env python3
"""Board checks for the real recovery path, no streaming or VPU access."""
import os
from pathlib import Path
import subprocess as sp

ROOT = Path(__file__).resolve().parents[2]
RESTORE = ROOT / "tools/k2b-restore-desktop.sh"
assert RESTORE.is_file(), "FAIL: independent restore script is missing"
assert os.geteuid() == 0, "run with sudo"
before = sp.check_output(["pgrep", "-x", "xfce4-session"], text=True)
# /dev/disp final close disables output: this deliberately exercises the
# known no-signal condition, then restores it, without touching DMA or VPU.
fd = os.open("/dev/disp", os.O_RDWR)
try:
    held = sp.run(["/bin/sh", str(RESTORE)], capture_output=True, text=True)
    assert held.returncode != 0 and "busy" in held.stderr, held
finally:
    os.close(fd)
for attempt in range(2):
    sp.run(["/bin/sh", str(RESTORE)], check=True)
    assert sp.check_output(["pgrep", "-x", "xfce4-session"], text=True) == before
print("PASS: occupied display refused; no-signal recovered; repeat safe; XFCE PID unchanged")
