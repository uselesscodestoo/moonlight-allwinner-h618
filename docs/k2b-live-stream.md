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

### Audio and campus-network follow-up (17:40–18:00)

Sunshine's local `stream_audio=disabled` setting explained the original absence
of all audio packets. The user enabled it and restarted Sunshine personally.
Packets then arrived, exposing repeated ALSA EAGAIN/short-write errors and XRUN.
K2B now uses a blocking ALSA writer on common-c's dedicated audio decoder thread,
finishing partial writes rather than dropping remaining PCM samples. Other
platforms retain their original nonblocking/direct-submit selection. Both the
K2B native build and the existing backend-disabled baseline build passed.

The user clarified that headphones were connected to the board's analog jack.
A run with `-audio hw:0,0` reported RUNNING, 48 kHz S16 stereo, 1771200 frames
written and 8 recoveries. Nevertheless, the user heard nothing. Direct board
`aplay -D plughw:0,0` of a known WAV was also inaudible. **Audio remains unaccepted
and further audio work was explicitly deferred by the user.** No mixer settings
were changed. The ALSA change is only a tested write-path improvement, not a
claim that analog or HDMI sound is working.

Statistics now include monotonic `elapsed_ms`, recoveries, discarded queued
access units and queue peak. `tools/k2b-log-summary.ps1 -Log PATH` calculates
rates from measured intervals and explicitly does NOT label them presented FPS.
It rejects old logs without elapsed time. The analog test's measured window was
52.667 seconds / 3045 decoded frames = 57.816 fps, with 17 reported network-drop
events. This is not a 60 fps pass.

A 650-second planned run used 1080p60 / 8 Mbps / ALSA null to focus on video.
It ended early after 335.290 seconds when the control connection disconnected.
Final counters: received=11602, decoded=11590, display_submitted=11590,
recoveries=1, queue_discarded=8. Across the logged 330.281-second window the
decoded rate was 34.353 fps; 1975 network-drop log events were reported. These
events are not a count of independently measured missing HDMI frames.
The kernel recorded WLAN leaving COMPLETED at 17:58:07 and reassociating. The
Sunshine log recorded disconnect at 17:58:14. Normal video cleanup ran, no new
DE/IOMMU fault was captured, boot ID stayed unchanged and no moonlight process
remained. **The ten-minute stability gate was NOT passed.**

Wi-Fi power saving was experimentally disabled at 17:54:04; losses persisted.
A brief baseline compile also occurred during this trial, so it is not a clean
CPU-controlled power-saving benchmark. Original power saving was restored after
the failed run. No persistent network profile change was made. With permission,
a local 1080p60 ffplay test pattern with frame numbers was opened at 17:55:54;
it was closed after the stream ended. Its host display was 165 Hz, so this was
a dynamic-content exercise, not a calibrated HDMI frame-count measurement.

Evidence: `k2b-live-audio-20260924.log`, `k2b-live-analog-20260924.log`,
`k2b-soak-8mbps-20260924.log`, `k2b-soak-kernel-20260924.log`, and
`k2b-wifi-powersave-20260924.log` under `F:/temp/projects/`.
The user is arranging a more stable network. Do not restart board tests until
that change is ready; recheck both endpoint IPs before the next live run.

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
