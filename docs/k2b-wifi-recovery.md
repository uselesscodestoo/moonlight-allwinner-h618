# K2B campus Wi-Fi recovery checkpoint (2026-09-24)

## Observed failure and scope

The user's `F:/temp/full_log.txt` shows complete video input stopping while
input/control remained usable. At 405--430 seconds the counters stayed at
received=20651, decoded=20650, display_submitted=20650, queued=0, held=1.
There are 1728 unrecoverable-frame reports, beginning at frame 20652, and 19
IDR requests. This supports starvation of complete access units, not a proven
VPU/DE33 hang. The exact blocking point during the user's delayed quit cannot
be established from this log alone.

The board also caps ordinary receive-buffer requests with
`net.core.rmem_max=229376`. UDP receive-buffer errors were observed, but the
cumulative counters do not prove that every original lost packet had this cause.

## Changes

- Preserve actual 1920x1080/60 hardware decoding, direct DMA-BUF display,
  display-buffer ownership, and the previously accepted low-latency SBM gate.
- K2B's worker requests an IDR after 2 seconds without a complete access unit,
  then at 4, 6 and 8 seconds. After 10 seconds it signals the existing main
  thread's termination path and performs normal cleanup. Incoming fragments do
  not reset this deadline. This is not a watchdog for a blocked kernel ioctl.
- K2B ALSA uses nonblocking writes with at most 100 ms of waiting per packet,
  10 ms wait slices and an atomic stop flag. Cleanup drops pending audio instead
  of draining it. Other backends retain their existing behavior.
- For K2B only, honor the common library's requested per-socket receive-buffer
  size with SO_RCVBUFFORCE if ordinary SO_RCVBUF was silently capped. This
  requires the privilege already used by the board launcher; failure is logged
  and leaves the original buffer in place. No global sysctl is changed.
  The executable exports setsockopt because common-c is a shared library:
  linker --wrap alone did not intercept its calls in the initial experiment.
- Return from failed connection startup instead of entering the input loop
  after common-c has already unwound its partially started streams.

## Verification

Tests used campus Wi-Fi: board `172.31.197.223`, host `172.31.193.248`, no wired
connection. No Wi-Fi power-save, interface, firewall or host configuration change
was made. Deliberate losses occur only inside a separate diagnostic executable,
on video packets from UDP port 47998. The normal executable has no loss hook.

| Test | Result |
| --- | --- |
| Fake-clock complete-AU starvation | Old code failed; new code requests IDR at 2/4/6/8 s and terminates at 10 s |
| Mock ALSA normal/wait/stop/cleanup | Old wait, stop and drain cases failed; all four new cases pass |
| Native receive-buffer test | Ordinary request: 3145728 requested, 458752 actual; fixed: 6291456 actual (Linux doubled accounting) |
| Partial video loss after 10 s | Drop 3/4 packets while continuing to deliver fragments; 599 frames decoded/displayed, then normal automatic shutdown after 10 s without a complete AU; no outer timeout needed |
| Four-second video outage | IDR requests followed by resumed decoding/display; later windows advance 300 frames per 5 s; normal cleanup |
| Deliberately rejected runtime startup | Returns in about 1 s instead of waiting in the input loop until the old 35 s test timeout |
| Normal production launcher, 75 s limit | Worker elapsed 69.717 s; 4159 decoded, 4158 display submissions; steady statistics window 64.717 s / 3883 decoded and submitted = 60.000 fps |

The short-outage run had one input-queue recovery before injection. The normal
run had no video recovery, queue peak 2, and one pending frame discarded at
shutdown. Its audio log includes initial packet loss and 24 ALSA recoveries;
this is not a claim of zero audio underruns. The user confirmed that picture,
sound, keyboard/mouse and latency were all normal in the production run.

The live video socket showed receive buffer 5767168 bytes and zero socket drops;
another smaller socket showed 41 drops. Global UDP buffer errors increased by
41, so this is not a claim that all UDP loss has disappeared. Global rmem_max
remained 229376. DMA-BUF accounting after the tests was 0 objects / 0 bytes.

Final native build and frame (59), AU (272), queue, picture (319), audio,
predecode/watchdog and normal/timeout display-retirement tests passed.
Production binary SHA-256:

```text
365086d2b6beb2d8ba49233df218318b614699c70fcf515f41f3d54f3f7e1fbf
```

Logs on the development PC, under `F:/temp/projects/`:

- `k2b-wifi-partial-20260924.log`
- `k2b-wifi-short-20260924.log`
- `k2b-wifi-init-failure-fixed-20260924.log`
- `k2b-wifi-fixed-normal75-20260924.log`

The corresponding board logs are under `build/k2b-tests/`. The prior production
binary is retained as `build/k2b-integrated/moonlight-before-wifi-fix-cd42eb2`.

## Usage and limits

```sh
cd ~/projects/moonlight-embedded
sudo sh tools/k2b-stream.sh 172.31.193.248
```

A short outage can recover within the same session. Persistent absence of
complete video for 10 seconds ends the session; rerun the command to reconnect.
There is no automatic reconnect, guarantee against every type of hardware hang,
or proof that the original random campus-network incident is fully eliminated.
Unrecoverable missing packets cannot be reconstructed by this patch.

To repeat controlled fault tests (not needed for normal use):

```sh
sh tests/k2b/build_video_loss.sh
sudo timeout -s INT -k 15 35 sh tests/k2b/run_video_loss.sh 172.31.193.248 short
sudo timeout -s INT -k 15 35 sh tests/k2b/run_video_loss.sh 172.31.193.248 partial
```

The timeout is an outer diagnostic safeguard; the persistent-loss test completed
via its normal termination path without reaching that safeguard.
