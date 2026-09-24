# K2B live streaming checkpoint — 2026-09-24

Independent `k2b-cedarc-disp` branch, KICKPI K2B Longan Linux 5.4.125.
Following the user's request to prioritize functional tests over repeated reviews,
this checkpoint uses the vendor release-fence path and measured live runs. It is
not a claim that all vendor RCQ races have been formally excluded.

## What ran

Board: `kickpi@172.31.197.223`; Sunshine: `172.31.193.248` (MyWin).
Project-specific pairing succeeded; Desktop is app 2. Private keys remain only
in the board's `build/k2b-pairing` directory and are not committed.

`-platform k2b -1080 -fps 60 -codec h264 -bitrate 15000 -app Desktop` uses:

- CedarC H.264 hardware decoding with the custom DMA-BUF memory adapter.
- NV12 allocation 1920 x 1088, stride 1920, visible crop 1920 x 1080.
- The same DMA-BUF imported into vendor `/dev/disp`, ch0/layer0, HDMI mode 10.
- No decoded-pixel memcpy, software decoder, GL upload or RGB conversion.
- Compressed access units are copied into Cedar's stream buffer.

Native integrated build and a blank-commit/fence smoke test passed. First live
run submitted 18 frames, then overflowed its input queue and logged DE IOMMU
faults. The user saw no signal. A diagnostic holding the first decoded picture
then ran at approximately 60 decoded fps; the user confirmed seeing the image.

Continuous submission became stable by allowing at most two outstanding pictures and
waiting for the oldest vendor release fence before another flip. Merely holding
four pictures while making bursty SET_CONFIG2 calls was not sufficient. The next
20-second run submitted 1056 pictures; the user confirmed desktop display.
Two IOMMU faults remained at exit. Retaining the last picture's fence and waiting
for it after the first blank commit fixed the observed retirement failure.

The 65-second invocation (including startup and shutdown) produced:

| Counter | Final value |
| --- | ---: |
| Received / enqueued access units | 3735 / 3735 |
| Submitted to Cedar | 3733 |
| Decoded / submitted to display | 3731 / 3731 |
| Held at pre-cleanup statistics | 2 |
| Accumulated DecodeVideoStream call time | 18378 ms |

Stable five-second intervals advanced by about 300 pictures. Two running display
snapshots showed timeline current 886 -> 2526, both with composer skip=0,
DMA import cache=2 (maximum 3), HDMI 1920x1080 around 60 Hz, and an enabled NV12
1920x1080 crop. Existing display err=10/skip=28 did not increase between those
snapshots. No new DE/IOMMU fault appeared in that entire run or its cleanup.
After exit no moonlight process remained; CMA free was 83844 KiB of 131072 KiB.
Timeout sends TERM only, without a KILL escalation; its nonzero exit is expected.

The user then reported **no signal during the 65-second test**, while the board's
G102 mouse successfully and smoothly controlled the host. Therefore this run is
NOT accepted as a visible-display pass despite healthy software counters. Earlier
static and 20-second tests were visually confirmed, but repeated-start HDMI
reliability still needs resolution. These observations establish approximately
60 decoded/submitted frames per second, not independently measured 60 distinct
visible frames each second. Audio and longer soak testing also remain. Audio reported
`No audio traffic was ever received from the host`; ALSA initialization alone
does not establish audible output. G102 input was confirmed by the user, not
merely inferred from USB enumeration.

### Reconnect and recovery follow-up

A subsequent 60-second view-only run submitted 3456 pictures and was visually
confirmed. During it HDMI reported HPD=1, RxSense=1, PhyLock=1, PhyPower=1,
AVMute=0 and 1080P60. Another input-enabled attempt overflowed the queue after
41 pictures: the old fail-stop path closed HDMI while leaving input connected.
This concretely reproduces a no-signal-with-working-mouse failure mode; it does
not prove the earlier 3731-picture observation had exactly the same cause.

The backend now drains the displayed pictures, resets Cedar, discards queued
compressed data and waits for a fresh IDR after queue rejection. It keeps the
display client open through this recovery. A deliberate drop of AU 120 exercised
the path on the board: `recovery 1 reset complete`, then `IDR accepted`. The
60-second run submitted 3424 decoded pictures total and continued at about
300 pictures per five seconds after recovery. The user confirmed **both picture
and board mouse normal**. HDMI remained locked, composer skip=0; cleanup logged
no new DE/IOMMU error, no moonlight process remained, and CMA free was 82440 KiB.

`K2B_DIAGNOSTIC_DROP_FRAME=1` enables that one-shot compressed-frame-loss test.
It must be unset for ordinary use. Recovery temporarily interrupts video; this
is not a no-drop/no-latency guarantee. Audio still received no host traffic and
was not accepted. Full keyboard/gamepad coverage and longer soak tests remain.

## Build / run on this board

```sh
cd ~/projects/moonlight-embedded
cmake -S . -B build/k2b-integrated \
  -DENABLE_K2B=ON -DENABLE_X11=OFF -DENABLE_CEC=OFF -DENABLE_PULSE=OFF \
  -DK2B_CEDARC_ARCHIVE="$HOME/projects/cedarx_test/tina-243f2cbe.tar.gz" \
  -DK2B_VENDOR_CEDAR_HEADERS="$PWD/build/k2b-vendor-headers/drivers/media/cedar-ve" \
  -DK2B_VENDOR_HEADERS="$PWD/build/k2b-vendor-headers/include"
cmake --build build/k2b-integrated -j4
# After reboot, if this node does not exist:
test -c /dev/cedar_test_heap || sudo insmod ~/projects/cedarx_test/module/cedar_test_heap.ko
sudo sh tools/k2b-stream.sh 172.31.193.248
```

Use only the matching 5.4.125 exporter module. No system library installation or
kernel replacement was made. The wrapper uses the private runtime and existing
pairing directory, HDMI ALSA `hw:1,0`, and bundled controller mappings. Additional
Moonlight options may follow the host argument, e.g. `-viewonly`.
Pairing a different host must be done separately using the same key directory.

Ctrl+Alt+Shift+Q or TERM normally stops streaming. The vendor driver may power
HDMI off after the last `/dev/disp` close; subsequent runs re-enable the existing
1080p60 configuration. Desktop restoration is not claimed.

`K2B_DIAGNOSTIC_STATIC=1` is a diagnostic only: it holds the first picture while
decoding/discarding later output. Do not set it for normal use or fps acceptance.
AU rejection now attempts in-session IDR recovery; decoder/display failures
still stop video. Do not use SIGKILL to clear a
hardware-stuck run. Ambiguous failed cleanup requires board recovery, not an
assumption that leaked userspace pointers survive process exit.

## Evidence on the development PC

`F:/temp/projects/` contains `k2b-live-first-20260924.log`,
`k2b-live-static-20260924.log`, `k2b-live-paced-20260924.log`,
`k2b-live-minute-20260924.log`, `k2b-live-running-display-20260924.log`, and
`k2b-live-kernel-20260924.log`. Follow-up evidence includes
`k2b-live-hdmi-diagnostic-20260924.log`, `k2b-hdmi-active-state-20260924.log`,
`k2b-live-combined-20260924.log` (failed 41-frame attempt),
`k2b-live-recovery-20260924.log`, and `k2b-recovery-kernel-20260924.log`.
Kernel logs were captured continuously over SSH across the corresponding tests.
