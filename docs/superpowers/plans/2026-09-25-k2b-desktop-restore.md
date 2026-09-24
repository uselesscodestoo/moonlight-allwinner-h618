# K2B Desktop Restore Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to execute inline; the user authorized proceeding directly after planning.

**Goal:** Run the unchanged Moonlight binary under systemd and restore XFCE after its service stops.

**Architecture:** A reusable managed-session shell wrapper creates a single transient service with `ExecStopPost`. The existing launcher supplies the original command and Pulse environment. A separate recovery shell script uses the verified debugfs operation and a small fb0 ioctl executable.

**Tech Stack:** POSIX shell, systemd 249, Python standard-library integration tests, C/fbdev, existing native CMake build.

**Execution status (2026-09-25):** Implementation and verification below are complete;
both real-stream recovery runs were visually accepted by the user. Final commit
and board metadata synchronization are the remaining handoff operations. Detailed
results and log paths are in `docs/k2b-live-stream.md`.

## Task 1: Prove lifecycle behavior before hardware use

- [x] Create `tests/k2b/test_managed_session.py` and shell fixtures under `tests/k2b/session/`. Run actual systemd services, not a mocked systemd. Assert post-processing via journal messages for normal exit, exit 7, TERM, KILL, exec failure, stopped launcher, and killed launcher. Confirm arguments containing spaces and dollar signs remain literal, Pulse values propagate, and duplicate launch does not stop the original service.
- [x] Run `sudo python3 tests/k2b/test_managed_session.py`; first require a clear failure because the managed-session entry point does not exist.
- [x] Implement `tools/k2b-managed-session.sh UNIT POST_SCRIPT COMMAND [ARGS...]` using `systemd-run --wait --collect --service-type=exec`, `ExecStopPost=/bin/sh POST_SCRIPT`, `SendSIGKILL=no`, `TimeoutStopSec=15s`, and journal output. Forward INT/TERM/HUP as a service stop request; hold a per-unit flock, preserve exit result and follow logs without making recovery depend on the launching terminal.
- [x] Repeat the same test command until all lifecycle checks pass. Never use the actual Moonlight process in signal-kill tests.

## Task 2: Recover desktop and wire the normal launcher

- [x] Add `tools/k2b-fb-unblank.c`, building `k2b-fb-unblank` only for ENABLE_K2B in `CMakeLists.txt`. Its operation is `open("/dev/fb0", O_RDWR|O_CLOEXEC)`, `ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK)`, close, with nonzero status on failure.
- [x] Add `tools/k2b-restore-desktop.sh`: refuse active Moonlight/disp owners; send `disp0`, `blank`, `0`, `1` to the four debugfs controls, then invoke the fb0 helper; require the enabled desktop layer and powered/locked HDMI on readback. Preserve mode and the XFCE session. Keep this executable usable manually.
- [x] Update `tools/k2b-stream.sh` to call the managed-session wrapper with unit `moonlight-k2b`; retain the current private-runtime command, host/options, pairing, mappings and Pulse route. Video/audio source unchanged; CMake refreshed the embedded Git version and relinked main while building the helper, without changing decoder behavior.
- [x] Sync exact changed files to the board, run `cmake --build build/k2b-integrated -j4`, `sh -n tools/k2b-*.sh`, repeat managed-session tests and verify recovery refuses an occupied disp device without toggling it.

## Task 3: Board acceptance and handoff

- [x] Run manual restore twice with no streaming, verify HDMI stays 1080p60 RGB and original Xorg/XFCE PIDs remain.
- [x] Run a short normal AVC session, stop normally through systemd and inspect post-processing and DMA-BUF cleanup; repeat with INT to the launch wrapper. User confirmed desktop recovery after both runs; no separate subjective audio/latency confirmation was received for these runs.
- [x] Update `docs/k2b-live-stream.md` with normal start, stop, manual recovery, journal commands and the kernel-hang/forced-kill limitations. Mark the approved design status as accepted.
- [ ] Run fresh tests and diff checks, commit only this task's changes on `k2b-cedarc-disp`, and synchronize matching files/commit to the board, preserving its existing launcher executable-bit change. No unrelated merge or remote push.
