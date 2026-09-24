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

### Direct Ethernet follow-up (18:07–18:14)

The user connected the board directly to the PC. DHCP provided board eth0
192.168.137.73/24 and PC Ethernet 192.168.137.1/24; WLAN remained connected.
`ip route get 192.168.137.1` selected eth0. Three pings measured 0.896–2.291 ms
without loss. No network-sharing, firewall, or interface setting was changed
by this test. The stream terminates on the PC's local Ethernet address, rather
than requiring Internet-sharing NAT forwarding.

The user's plain `aplay /usr/share/sounds/alsa/Front_Center.wav` had been audible.
Default ALSA uses the user's PulseAudio server, whose AudioCodec-Playback sink
opens hw:0,0. A 90-second 1080p60/15 Mbps run used ALSA `pulse` with the existing
user server and cookie path supplied to the root process. Connection succeeded;
the user confirmed picture and mouse, but no audio and noticeably higher latency.
Measured window: 4978 decoded/display-submitted frames / 83.002 s = 59.974 fps.
There was one startup network-drop event, no worker recovery, and 5 queued AUs
discarded at normal shutdown. Video queue occupancy was usually 4–5 frames.
Audio logged 370320 written frames and 8 recoveries; audibility did NOT pass.

A 60-second comparison with `-audio null` measured 3180 frames / 53.033 s =
59.963 fps, zero reported network drops and zero worker recoveries. Queue
occupancy was mostly 0–2, with one logged 3; 2 pending AUs were discarded on
shutdown. The user confirmed reduced latency. This is suggestive, not a causal
latency measurement: startup backlog differed, and the first ffplay window had
failed to auto-exit and overlapped the comparison. Both owned windows were
explicitly closed afterwards. Software rates are NOT measured HDMI presentation
rates. Both streams cleaned up; CMA free returned above 80 MiB.

Plain board `aplay` of the same Front_Center sample during the null-audio stream
was also inaudible according to the user. Another replay after stopping was
performed; listening confirmation remains pending. PulseAudio reported a roughly
683 ms configured sink latency during the local WAV test. A low audio write count
also occurred with ALSA null, so write counts alone do not prove output blocking.
No mixer or audio-server configuration was modified. Audio remains unresolved;
the relationship between display operation and local audibility is unproven.

Evidence: `k2b-wired-pulse-20260924.log`, `k2b-wired-null-20260924.log`,
`k2b-wired-audio-idle-20260924.log` in `F:/temp/projects/`.

### Completed wired duration test (18:15–18:26)

Unchanged code at 766b513 ran H.264 / 1920x1080 / requested 60 fps / 15 Mbps
with `-audio null` for 618.159 seconds of worker lifetime (620-second TERM
timeout including setup). ALSA null discards playback; audio transport and
decoding still run. A single owned ffplay testsrc2 1080p60 window supplied
dynamic content. No builds, driver changes or audio configuration changes were
performed during the run. The user confirmed the ongoing picture was always
normal during the long test.

Final received=36811, decoded=36806, display_submitted=36806, recoveries=0.
The measured statistics window is 36538 decoded frames / 613.150 seconds =
59.591 fps, not an exact sustained 60 fps pass. No network-drop log events were
reported. Several intervals had lower received rates (roughly 53–58 fps), with
correspondingly lower decode rates; these cannot be assigned to VPU throughput
without further source/capture evidence. The host test process remained alive.

Input queue occupancy initially stayed around 0–2, then reached 5–6 around seven
minutes, before lower incoming rates drained it. Peak occupancy was 7; no queue
overflow or reset occurred. Three pending AUs were discarded on normal shutdown.
This reproduces persistent backlog without PulseAudio playback, so the earlier
audio-on/off comparison does NOT establish audio as the cause of video latency.
The worker currently gates decode as well as display submission on display
retirement; separating preparation from safe display submission is a follow-up
hypothesis, not an implemented or verified fix.

Sampled CPU use was about 12.6–14.4% (ps process CPU), RSS 29356–29880 KiB,
temperature 54–58 C and running CMA free about 42 MiB. Display importer cache
stayed at 2 (max 3), composer skip stayed 0, manager err stayed 0 and historical
output err=10/skip=79 did not change across samples. Ethernet RX errors stayed
at the pre-existing 3. The captured new kernel log contains no IOMMU fault,
Oops or crash. Shutdown completed, no moonlight process remained, boot ID was
unchanged and CMA free returned to 78616 KiB. HDMI powered down after exit, as
previously accepted. The file named `final-active` was actually sampled after
automatic exit and must not be cited as an active-HDMI failure.

The ten-minute connected-runtime test is now completed, with positive user
visual feedback. Exact per-frame HDMI presentation and audio remain unverified;
this does not satisfy every final acceptance requirement. Evidence is in
`F:/temp/projects/k2b-wired-soak-20260924.log`,
`k2b-wired-soak-summary-20260924.json`, `k2b-wired-soak-kernel-20260924.log`
and the matching start/mid/five-minute/seven-minute/nine-minute/exit-state logs.

### Predecode experiment: lower latency, unsafe exit (18:35–18:39)

An uncommitted candidate allows one decoded pending picture while two pictures
are held by display. It retains the existing fence gate on display submission;
stop/reset return the pending picture separately. A test running the real worker
with fake hardware boundaries failed on the baseline (`decodes == 1`, actual 0)
and passed after the change, including bounded preparation, no early display
commit, and return of the pending picture on stop. Frame/AU/queue/picture tests,
native build and the native scheduling test passed. These tests do NOT model
the vendor's physical RCQ/scanout retirement.

A 40-second stream decoded 2249 / submitted 2248 pictures. A following 90-second
test injected the existing AU-120 drop, recovered once and decoded 5260 /
submitted 5258 pictures; its measured decode window was 83.214 s at 59.990 fps.
The user explicitly confirmed normal functionality and noticeably lower latency.

**Both exits produced DE invalid-address / IOMMU faults. This candidate is NOT
accepted for normal use.** The first exit logged six invalid-address events near
kernel uptime 53992.87–53992.96; the second logged three near 54114.41–54114.45.
The first failure was discovered in the full kernel follower only after the
second test was already running; earlier short `dmesg | tail` checks missed it.
Future runs must inspect the entire exit interval before restarting. Both test
processes have exited; do not run more hardware tests until board recovery.

At this checkpoint candidate source remained in the working tree for the
retirement fix. The normal board binary was restored from
`moonlight-baseline-6720795` (SHA256
4fc8586b15cac5b68e733321571ebbd47abeee567a9084f07d91620a98cbeb51).
The failed candidate binary is preserved as `moonlight-predecode-20260924-1835`
beside it. Do not run that archived failed binary. The subsequent retirement
fix and current binary are described below. Evidence:
`k2b-predecode-short-20260924.log`, `k2b-predecode-recovery-20260924.log`, and
`k2b-predecode-kernel-20260924.log` in `F:/temp/projects/`.

### Predecode retirement fix after reboot (18:45–18:49)

The user explicitly authorized reboot. The boot ID changed to
`d031618f-d45e-49a3-b677-d3cd21721271`; wired/WLAN addresses were unchanged,
CMA remained 128 MiB and the matching exporter module was reloaded.

Vendor `disp_mgr_set_layer_config2()` can unmap older imports before applying
its new layer configuration. The presenter now retains a duplicate of the
preceding picture's fence and waits for it BEFORE the first blank commit on
retirement. This establishes the last flip's timeline progress before starting
the existing three-blank drain; it does not change normal display submission
limits or release pictures early. Timeout returns an error without issuing the
blank. This remains a practical vendor timeline sequence, not an independent
proof of physical RCQ completion in all circumstances.

The real-presenter ordering test failed before the change and passed after it,
including the timeout/no-blank case. Native build, native retirement test,
predecode scheduling test and 396 display-config checks passed. The fix is a
source-backed timing hypothesis with the following hardware regression evidence:

- 25-second launch, worker elapsed 24.190 s: decoded/submitted 1410 pictures;
  measured window 1151 / 19.190 s = 59.979 fps. No network-drop reports.
- 65-second launch with the AU-120 diagnostic rejection: recovered once,
  worker elapsed 63.146 s, decoded 3679 / display-submitted 3677; measured
  window 3416 / 58.146 s = 58.749 fps. Received rates also varied below 60 in
  this run, so it is a recovery/cleanup pass, NOT an exact 60 fps pass.
- Full post-exit kernel logs were checked before any subsequent launch.
  Neither exit nor the injected in-session reset produced DE invalid-address,
  IOMMU or Oops reports. No moonlight process remained; final CMA free was
  117528 KiB. The pre-fix repeated exit faults were not reproduced in these runs.

Current board binary SHA256:
`de166f63c065984b36e1e3628e58faeaaaf58c6775ab2bafc261e18ef8587a04`.
It includes predecode plus the retirement fix. Both older baseline and failed
candidate binaries remain archived; current source builds the fixed candidate.
At that checkpoint, new-version long-duration validation, quantitative
presentation/latency evidence and audible audio remained outstanding. The audio
result below supersedes the audible-audio gap. Previous user feedback established the
predecode latency benefit, not a separate optical measurement of this fix.

Evidence prefix: `F:/temp/projects/k2b-predecode-retirefix-`, including
`short-20260924.log`, `recovery-20260924.log`, `kernel-20260924.log`,
`full-kernel-20260924.log`, and `recovery-full-kernel-20260924.log`.

### Headphone streaming audio confirmed (19:05)

The user first confirmed hearing the board's local `Front_Center.wav`, without
streaming. A subsequent real Sunshine session played the same WAV from the PC
three times through `System.Media.SoundPlayer`, then through Moonlight's existing
ALSA backend and the desktop user's Pulse sink `AudioCodec-Playback` (`hw:0,0`).
The user confirmed normal headphone speech AND Windows volume-change sounds.
This is an audible streaming-audio pass, not merely successful PCM writes.

An ALSA file/slave-pulse diagnostic captured 492960 stereo frames (10.27 s),
mean -22.6 dBFS and peak -4.7 dBFS. The local source has mean -22.6 dBFS and
peak -6.5 dBFS; this capture also includes Windows sounds, so it is not a
sample-perfect comparison. Twelve ALSA recoveries were logged across intermittent
host audio, but the user reported normal sound. Continuous-audio soak remains
separate. The previous ffplay sine test was extremely quiet (-56.2 dBFS peak);
that different source/player must not be used to infer a generally silent or
attenuating Moonlight decoder. No gain boost or decoder change was made.

The launcher now defaults to ALSA `pulse` and discovers the invoking sudo user's
Pulse socket/cookie paths. Explicit Pulse environment settings are preserved;
cookie contents are never printed. This uses the ALSA Pulse plugin, so the
build can keep `ENABLE_PULSE=OFF`. No system mixer/Pulse/Sunshine settings were
changed. `-audio hw:1,0` still selects HDMI and `-audio null` discards playback.
HDMI audio is not claimed as an audible pass. If no desktop Pulse session exists,
start it normally or explicitly choose a direct ALSA device; the script does not
start a sound server or silently fall back to another output.

`sudo sh tests/k2b/check_stream_launcher.sh` checks default/override arguments
and automatic Pulse environment on this board without starting hardware. It
failed against the previous launcher and passed after the change. Actual `sh`
launching is separately tested on the board; the argument test uses Bash only to
intercept `exec`. Evidence: `F:/temp/projects/k2b-audio-speech-20260924.log` and
`k2b-audio-isolation-20260924.md`, plus the two earlier capture logs/configs.

A subsequent 70-second launch used only
`sudo sh tools/k2b-stream.sh 192.168.137.1`, without manually supplied Pulse
variables or diagnostic audio capture. Pulse reported the root Moonlight client
on sink 1, stereo 48 kHz, unmuted at 100%, sampled sink latency 20 ms. The user
confirmed **audio, picture and mouse all normal; interaction latency acceptable**.
Worker elapsed 68.245 s, received 3888 / decoded 3887 / display submissions 3887,
video recoveries 0, queue peak 1. Source cadence varied on the desktop, so this
is not an exact sustained-60/presentation measurement. Audio wrote 399120 frames
with 16 recoveries during intermittent sounds; uninterrupted audio remains to be
tested. Normal cleanup completed, no process remained, CMA free was 117100 KiB,
and full current-boot kernel error checks found no DE invalid-address/L2 Page/
Oops/BUG matches. Evidence: `k2b-audio-default-launcher-20260924.log`.

### Updated-version A/V and real-game soak (19:14–19:25)

Current code/launcher checkpoint `9e42e99`, unchanged native binary SHA256
`de166f63c065984b36e1e3628e58faeaaaf58c6775ab2bafc261e18ef8587a04`,
completed a 625-second timed launch using the ordinary headphone-default command.
The video worker ran 623.179 s. Initial load was a 1080p60-generated ffplay window
plus a quiet looping test tone. During the run the user closed the test window
and began playing a game; the owned tone process was then stopped as requested.
The remainder is a real-game workload, not an uninterrupted synthetic test.

The user confirmed everything normal both during play and after the full run:
picture, game sound, board keyboard and mouse; no reported stalls, audio breaks
or in-session signal loss. Keyboard receiver and G102 hotplug also appear in the
kernel/stream logs. No test windows or tone processes were left behind.

- Received 35957 AUs, decoded 35956 and display-submitted 35956 pictures.
  Measured sample window: 35728 decoded / 618.174 s = **57.796 fps**.
  Incoming cadence varied similarly, while decoded/submitted tracked received.
  This is a ten-minute functional stability pass, NOT a claim of sustained
  exact 60 fps or independently measured physical HDMI presentation.
- Video recoveries 0, queued-AU discards 0, reported network-drop events 0.
  Queue peak 4 (including startup), sampled running queue mostly 0–1;
  no accumulating compressed-video backlog. Total decode-call time 191256 ms
  (about 5.32 ms per decoded picture), not end-to-end latency.
- ALSA wrote 26982000 stereo frames (562.125 s at 48 kHz) with **57 recoveries**
  across synthetic audio, a source transition and game sound. These counts
  must not be described as zero XRUNs or uninterrupted-source delivery.
  User listening nevertheless confirmed normal game audio. Sampled Pulse
  device + stream buffered latency was about 27–41 ms, not an optical A/V delay.
- RSS samples 30060–30496 KiB, process CPU 21.7–24.9%, temperature samples
  57.2–65.0 C. Running CMA free 78480–79084 KiB (about 76.6–77.2 MiB);
  display-import cache 1–3, maximum 3. Display manager/composer error/skip
  counters stayed 0; the outer HDMI skip counter stayed at its baseline 32.
- Normal cleanup completed; no remaining Moonlight process. Boot ID unchanged.
  CMA free was 114944 KiB immediately after exit, then 116948 KiB at the later
  sample versus 117044 KiB before the run. Whole-boot kernel logging through
  exit showed no DE invalid-address/L2 Page/Oops/BUG matches. HDMI power-off
  after exit remains the known vendor behavior, not an in-session failure.

Evidence in `F:/temp/projects/`: `k2b-av-soak-20260924.log`,
`k2b-av-soak-summary-20260924.json`, `k2b-av-soak-kernel-20260924.log`,
and `k2b-av-soak-{early-state,game-state,six-minute,eight-minute,nine-minute,exit-state}-20260924.log`.
At this checkpoint production-path fixed-sample pixel comparison and controlled/
independent actual presentation-rate verification remained unfinished. The pixel
comparison below closes the former for this sample, not the latter.

### Production-object pixel replay; paced replay not yet passed (19:33–19:42)

The experimental `tests/k2b/replay_video.c` is linked against the same
`video.c.o`, AU queue and presenter objects as the integrated Moonlight binary.
A link-time wrapper observes the real display-call boundary and delegates to
the real presenter. It does not replace the production executable. Hash mode
reads effective NV12 rows under DMA-BUF CPU-access synchronization; it therefore
adds deliberate diagnostic overhead and is not a throughput test.

Fixed input SHA256:
`ccb9c0860c510db82d97c002b4006ec66160b76e4d83845112965e0543575543`,
`~/projects/cedarx_test/samples/testsrc-1080p60.h264`, 600 H.264 AUs, no B frames.
Ten frames first matched a newly generated FFmpeg `-pix_fmt nv12` reference.
All **600 effective 1920x1080 NV12 frames then matched the existing software
reference**, with 600 distinct hashes. This compares decodes of the same lossy
bitstream, not the pre-encoding source. The user also confirmed correct-looking
test imagery. The first full run had four corrupted hash log lines because
stdout/stderr interleaved; the checker rejected it. Line-buffered single-line
records and separate output files fixed that test logging issue; the rerun passed.

Successful evidence: `F:/temp/projects/k2b-replay-hash600-lines-20260924.log`,
its `.stderr`, and `k2b-replay-reference600-20260924.framemd5`.
601 input AUs (one extra IDR after EOF to drain the streaming pipeline) produced
600 observed pictures, 19.856 s first-to-last, 30.167 submissions/s WITH hashing.
There were no video recoveries/discards or reported DE/IOMMU faults at exit.
`tests/k2b/check_replay_hashes.ps1` checks exact count, ordering and every hash.

The initial no-readback `pace` attempt stalled before its first picture:
received 5, submitted 1, decoded/displayed 0. It ended without normal cleanup.
An 8 MiB `cedar_test_heap` DMA-BUF remained attached to `1c0e000.ve`, verified
through `/sys/kernel/debug/dma_buf/bufinfo` after the process exited. No process
remained; hardware testing was stopped until reboot rather than reusing that state.
Failure evidence: `k2b-replay-pace600-20260924.log` and `.stderr`.

One test-tool shutdown defect was isolated without touching hardware: its
`signal()` calls linked to `__sysv_signal`, and a repeated-signal self-test died
with exit 130. Changing only the test tool to `sigaction()` makes repeated INT/
TERM self-tests pass. This establishes the signal-handler defect, not the root
cause of the no-picture stall. The user authorized necessary board reboots;
boot ID changed to `7fd6c59b-a203-4f90-b13f-968114321daa`, with zero DMA-BUF
objects and CMA still 128 MiB. The matching exporter module was reloaded.

### Paced startup fix and production-path retest

A short traced run proved `VideoStreamFrameNum` returned 8 after only one
submitted AU. Our arbitrary `>= 8` gate prevented the next AU from reaching
Cedar, while Decode returned no-bitstream and no picture. This run exited
normally through the corrected signal handler with zero DMA-BUF objects.
The count cannot be treated as the number of caller-submitted complete AUs.

The first experimental production fix removed that arbitrary threshold and retained the
actual `RequestVideoStreamBuffer` capacity check, the 8 MiB VBV and bounded
input queue. A CPU regression first failed against the old code, then passed;
it also checks that real buffer-allocation backpressure still stops submission.
The same ten-frame hardware test changed from 0 pictures to all 10 pictures.

Using the updated production objects, the no-readback 600-frame run produced
600 decoded and displayed submissions in **9.979548 s first-to-last**, or
**60.022760 submissions/s**. There were 601 input AUs including the drain AU,
zero recoveries/discards and queue peak 1. Submission intervals ranged from
7.532 to 25.832 ms, with one interval over 25 ms. These are user-space submission
intervals, not optical measurements of individual HDMI refreshes.
DE snapshots showed the active NV12 1920x1080 crop, current timeline progressing
39 -> 538 over the sampled run, composer skip 0 and manager error 0. Driver
reported HDMI 60.5–60.6 fps after its initial partial measurement; we do not
interpret that coarse driver number as an exact refresh-rate measurement.
After exit there were zero DMA-BUF objects and no DE/IOMMU/Oops/BUG matches.

The updated objects also passed a fresh **600/600 matching, 600 unique** NV12
hash comparison against the same software reference. Hash mode ran at 30.262
submissions/s because it intentionally reads and hashes every pixel.
Evidence: `k2b-replay-fixed-pace600-20260924.{log,stderr,disp}` and
`k2b-replay-fixed-hash600-20260924.{log,stderr}` in `F:/temp/projects/`.
The new integrated binary SHA-256 is
`25c03ceadb377ad9dcddd9364eea227db6559e598389ab9a5d37b0078d318567`.
The prior game-tested binary is preserved on the board as
`build/k2b-integrated/moonlight-pre-sbm-fix-edf986e` (SHA `de166f63...7a04`).

**The unrestricted change was not accepted for normal use.** In its normal-launch live
regression, the user confirmed sound, mouse and picture but reported much higher
latency. Logs showed a persistent 5–6 queued AUs, and submitted-to-decoded depth
of about 4, despite steady 60/s progress. A same-parameter run of the preserved
old binary also retained about 4 queued AUs and that decoder depth; simply
reverting the startup fix did not eliminate the measured backlog. The current
evidence does not establish the startup fix as the cause of increased latency.

The existing `K2B_DIAGNOSTIC_DROP_FRAME=1` experiment on the new build triggered
keyframe recovery and reduced queued AUs to 0–1, with steady 60/s thereafter.
There were two recoveries (one real initial queue overflow plus the intentional
AU-120 rejection), eight discarded queued AUs, and normal zero-DMA cleanup.
The user subsequently confirmed both the old-binary comparison and the recovered
new-binary run were fluid. Thus queue counters alone do not fully explain the
subjective difference; the unrestricted change is not treated as a completed
low-latency fix. Do not
enable this deliberate-drop diagnostic in the normal launcher or claim that
latency is fixed. Evidence: `k2b-sbm-fixed-live75-20260924.log`,
`k2b-sbm-old-live65-20260924.log`, and
`k2b-sbm-fixed-recovery65-20260924.log` in `F:/temp/projects/`.

The follow-up fix is narrower: bypass the count watermark **only until the
first decoded picture after initialization or decoder reset**, then preserve
the original steady-state threshold. The regression covers startup, actual
buffer-full backpressure, steady-state gating and post-reset startup. It failed
with the unrestricted version and passed with this narrowed implementation.
Its 600-frame paced replay produced all 600 pictures in 9.976891 s first-to-last
(60.039 submissions/s), no video recoveries/discards, queue peak 1 and zero
DMA-BUF objects after cleanup.
The subsequent ordinary-launch run passed user observation: picture, sound,
mouse and latency were all normal, with no diagnostic frame drop enabled.
Worker duration was 73.170 s; received 4367, decoded 4366, display-submitted 4365
(one pending picture returned at shutdown). The measured 68.170 s statistics
window advanced 4090 decoded/display-submitted frames, **59.997 fps**. Queue
stayed 0–1 after startup, peak 2; recoveries/discards/network-drop messages were
zero. ALSA wrote 38880 frames with zero recoveries in this intermittent-audio
run; that is not a continuous-audio endurance result. The preceding ten-minute
real-game audio/input soak remains the longer functional evidence.

Final narrowed build SHA-256:
`66d5a7726275b1332e1e738fae8ae6b985b3c7a2d4940c6d9921144dada44848`.
Its fresh hash-mode run again matched **600/600 frames, 600 distinct hashes**.
All these final runs cleaned up to zero DMA-BUF objects; the boot's kernel log
had no `invalid.*address`, `L2 Page`, `Oops`, or `BUG:` matches. CPU frame/AU/
queue/picture/predecode/retirement checks passed, including timeout retirement
and persistent signal-handler tests. Evidence is
`k2b-startup-only-live75-20260924.log`,
`k2b-startup-only-pace600-20260924.{log,stderr}` and
`k2b-startup-only-hash600-20260924.{log,stderr}` under `F:/temp/projects/`.
No claim of optical per-refresh timing or a new ten-minute soak on this last
startup-only change is made.

To reproduce the two distinct checks (hash mode is NOT a performance test):

```sh
sh tests/k2b/build_replay_video.sh
build/k2b-tests/replay_video --signal-check
sudo sh tools/k2b-runtime/run-private.sh build/k2b-integrated/tools/k2b-runtime/runtime \
  build/k2b-tests/replay_video ~/projects/cedarx_test/samples/testsrc-1080p60.h264 600 pace
# Replace pace with hash and keep stdout/stderr in separate files for comparison.
# K2B_REPLAY_TRACE=1 enables a bounded, observational Cedar API trace.
```

## Campus Wi-Fi recovery follow-up

See [the Wi-Fi recovery checkpoint](k2b-wifi-recovery.md) for the 2026-09-24
complete-frame starvation, bounded audio shutdown and per-socket receive-buffer
fixes, deterministic fault tests, and normal campus-network user acceptance.
Current campus host: `sudo sh tools/k2b-stream.sh 172.31.193.248`.

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
# Current directly connected PC; default is the tested board headphone route:
sudo sh tools/k2b-stream.sh 192.168.137.1
# Optional: HDMI audio (not yet audibly verified), or discard audio playback:
# sudo sh tools/k2b-stream.sh 192.168.137.1 -audio hw:1,0
# sudo sh tools/k2b-stream.sh 192.168.137.1 -audio null
```

Use only the matching 5.4.125 exporter module. No system library installation or
kernel replacement was made. The wrapper uses the private runtime and existing
pairing directory, headphone ALSA `pulse`, and bundled controller mappings. Additional
Moonlight options may follow the host argument, e.g. `-viewonly`.
Pairing a different host must be done separately using the same key directory.

## Managed sessions and XFCE restoration (2026-09-25)

The normal `tools/k2b-stream.sh` launcher now creates a transient systemd service
named `moonlight-k2b.service`. It is **not** an enabled boot service and does not
automatically restart or reconnect. Rebuild the K2B target once to produce the
small `build/k2b-integrated/k2b-fb-unblank` helper. The existing root launcher,
private libraries, pairings, 1080p60 AVC defaults and user Pulse route are retained.

Ctrl+Alt+Shift+Q exits Moonlight normally. Ctrl+C in the launching terminal asks
systemd to stop it. The same operation is available from another terminal:

```sh
sudo systemctl stop moonlight-k2b.service
sudo journalctl -u moonlight-k2b.service -n 80 --no-pager
# Standalone recovery after an old/unmanaged session has already stopped:
sudo sh ~/projects/moonlight-embedded/tools/k2b-restore-desktop.sh
```

`ExecStopPost` calls the independent recovery script even after an unexpected
service exit; it does not depend on the original launching terminal. Output is
stored in the journal and copied to the launching terminal. A failed launch or
failed post-hook returns failure; exact service exit codes remain in the journal.
The installed systemd may return a generic status 1 for a process that exits
nonzero immediately during startup, rather than that process's exact code.

The vendor's last `/dev/disp` close powers HDMI off. Recovery uses debugfs
`disp0 / blank / 0`, preserving existing timings and color format, then fb0
`FBIOBLANK(FB_BLANK_UNBLANK)` to restore the existing desktop layer. It never
restarts Xorg/XFCE and never holds `/dev/disp` open in the background. Driver
readback checks HDMI power/lock and the enabled desktop layer; optical correctness
still requires user observation.

Recovery refuses to act while Moonlight or another `/dev/disp` owner is active.
The wrapper also rejects overlapping managed sessions. systemd automatic SIGKILL
escalation is disabled. A still-running kernel-stuck process must not be treated
as a successful stop or have its DMA buffers forcibly recycled. No user-space
post-hook can guarantee recovery from a hung kernel, power loss, or damaged
driver state after a forced kill.

Hardware-free lifecycle regression: `sudo python3 tests/k2b/test_managed_session.py`.
fb0 ioctl regression: `make -f tests/k2b/Makefile test-fb-unblank`.
The board-only `sudo python3 tests/k2b/test_restore_desktop.py` deliberately
closes the final test disp handle to recreate no-signal, then checks recovery;
run it only with no streaming or other display tests in progress.

### Desktop recovery acceptance evidence

On 2026-09-25, all eight hardware-free systemd lifecycle tests passed (normal,
nonzero, TERM, KILL, worker exec failure, launcher INT, killed launcher, duplicate
launch). Argument and Pulse environment preservation passed. The fb0 helper's
success/open-error/ioctl-error cases passed; the two error messages in that unit
test are deliberate injected failures, not board device errors.

The board test rejected an occupied `/dev/disp`, recovered after the test handle's
final close, and successfully repeated recovery without replacing the XFCE session.
The real normal-stop session decoded 5042 frames in 84.465 seconds including
startup, with no video recovery/discard; systemd called the post-hook, DMA-BUF
accounting returned to zero, and the user confirmed the original desktop and
input were normal. A second session stopped by a 40-second SIGINT timeout decoded
2302 frames (worker 38.824 seconds); it also ran the post-hook and returned DMA-BUF
accounting to zero. Timeout status 124 is expected for that deliberately timed run.
The user also confirmed normal XFCE restoration after the second run.
Audio reported 1 and 3 recoveries respectively; zero audio underruns are not claimed.
Xorg PID 1329 and XFCE session PID 1477 were unchanged throughout these checks.

No video/audio/decoder source was modified. Reconfiguration refreshed the embedded
Git version in main and relinked the binary, so its SHA-256 changed to
`199e013639ab7c21028930a5955e2b8fc36a902d1f7bee8033066b1e6d354ac0`.
Logs: board `build/k2b-tests/desktop-restore-systemd-stop.log` and
`desktop-restore-launcher-int.log`; PC copies under `F:/temp/projects/` are named
`k2b-desktop-restore-systemd-stop-20260925.log` and
`k2b-desktop-restore-launcher-int-20260925.log`.

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
