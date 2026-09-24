# K2B HEVC checkpoint — 2026-09-25

Developed on k2b-hevc from the validated AVC/desktop-restore commit 6bdfe09,
then merged into the vendor-kernel main development branch k2b-cedarc-disp.
HEVC is now the default in the normal launcher and the K2B auto-codec preference.

## Run on the board

```sh
sudo sh ~/projects/moonlight-embedded/tools/k2b-stream.sh 172.31.193.248
# Optional AVC override:
sudo sh ~/projects/moonlight-embedded/tools/k2b-stream.sh 172.31.193.248 -codec h264
```

The script defaults to HEVC Main 8-bit. An explicit codec option overrides it.
Ctrl+Shift+Alt+Q exits normally; the existing systemd post-hook restores XFCE.
The main checkout keeps its existing ignored build/k2b-pairing directory;
no private key is committed.

Scope: HEVC Main 8-bit SDR, 1920x1080 at 60 fps, 15 Mbps. Main10/HDR and higher
rates are not enabled. The library comes from the same pinned CedarC archive:
libawh265.so, SHA256
6adc1bba9ca552e17f6bc8a5d4b28b6cba1a6268feebfded0fa99ed199457df1.
The private loader validates its path/provider and registers CreateH265Decoder
as a hardware decoder alongside H.264. Access units now accept VPS fragments.

The hardware output is NV12, visible 1920x1080, stride 1920, storage height
1088, allocation 3133440 bytes. The same DMA-BUF enters vendor DE33 /dev/disp.
There is no software decoding or decoded-pixel copy in the streaming path.
The AVC queue limits, predecode scheduling, release fences, recovery policy,
audio and desktop restoration are unchanged.

## Native build

```sh
cd ~/projects/moonlight-embedded
cmake -S . -B build/k2b-integrated \
  -DENABLE_K2B=ON -DENABLE_X11=OFF -DENABLE_CEC=OFF -DENABLE_PULSE=OFF \
  -DK2B_CEDARC_ARCHIVE=/home/kickpi/projects/cedarx_test/tina-243f2cbe.tar.gz \
  -DK2B_VENDOR_CEDAR_HEADERS=/home/kickpi/projects/moonlight-embedded/build/k2b-vendor-headers/drivers/media/cedar-ve \
  -DK2B_VENDOR_HEADERS=/home/kickpi/projects/moonlight-embedded/build/k2b-vendor-headers/include
cmake --build build/k2b-integrated -j4
sh tests/k2b/build_replay_video.sh
```

## Measured evidence

Focused tests were first observed failing on the original H.264-only code,
then passing after implementation: 281 AU checks, 261 loader scenarios,
H.265 selection plus Main10 rejection and predecode/watchdog regression.
Frame (59), picture (319), queue, display configuration (396), retirement and
fb-unblank checks also pass. Native pure-load tracing confirms both typed
hardware registrations without opening any device.

Fixed sample: PC-generated libx265 testsrc2, 600 distinct frames, 1920x1080,
60/1 fps, yuv420p, Main, 15 Mbps, no B-frames, IDR interval 60. Sample SHA256:
32e168c5eeb6076468a1edd811d269fd3ac2e4e88cb081a2a2fb616d0e4e67f9.
Board file: /home/kickpi/projects/k2b-hevc-testsrc-1080p60.hevc.

| Test | Result |
| --- | --- |
| HEVC paced production-object replay | 600 decoded/submitted, 59.997 display submissions/s |
| HEVC DecodeVideoStream call time | 2414 ms total, about 4.02 ms/frame |
| HEVC hash replay | All 600 visible NV12 frames exactly match software reference; 600 unique hashes |
| AVC regression in the same binary | 600 decoded/submitted, 60.023 display submissions/s |
| Real Sunshine HEVC session, 90-second command limit | H.265 selected; steady intervals advance about 300 pictures per 5 s |
| Cleanup | Managed restore reports HDMI locked and fb0 enabled; zero DMA-BUF objects afterward |

Hash replay maps/reads the pixels diagnostically and runs about 30 fps; it is
not the throughput test or the normal streaming path. Paced replay does not
read pixels. Decode-call time has millisecond resolution and is not end-to-end
latency. Submission cadence is not an independent optical measurement.

The live session was deliberately stopped by a bounded SIGINT timeout (no
SIGKILL escalation), and the service completed cleanup successfully. The
console's ENet interruption during intentional shutdown is not a mid-session
failure. Visual appearance, audio, controls and subjective latency still await
the user's confirmation for this HEVC checkpoint. No long gaming run or
HEVC-specific injected-loss recovery acceptance is claimed yet.

Original board evidence remains in ~/projects/moonlight-hevc/build:
hevc-replay-pace-2.log, hevc-replay-hash-1.log, avc-regression-pace-1.log,
hevc-live-1.log, and k2b-runtime-load.Dxz1Rd.
PC copies are F:/temp/k2b-hevc-*.log and
F:/temp/k2b-hevc-reference.framemd5.

## Vendor-mainline integration checkpoint

After merging into k2b-cedarc-disp, the launcher and K2B auto-codec preference
both default to HEVC. New checks first failed on the old AVC defaults, then
passed for default HEVC, explicit HEVC, explicit AVC, and all three tested
audio overrides (9 launcher combinations). Native rebuild and pure-load
validation of the merged checkout also passed.

A 30-second invocation of the normal launcher, with no codec argument,
selected H.265 and produced 1920x1080 NV12. Steady 5-second windows advanced
300 display submissions. Before cleanup it reported 1692 decoded and 1691
display-submitted pictures in 28.771 seconds including startup, zero recovery
events, and one queued AU discarded during intentional shutdown. The service
exited successfully and the desktop restore check passed.
Log: ~/projects/moonlight-embedded/build/k2b-hevc-default-smoke.log.
