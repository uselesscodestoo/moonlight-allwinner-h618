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
Production-path fixed-sample pixel comparison and controlled/independent actual
presentation-rate verification remain separate unfinished acceptance items.

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
