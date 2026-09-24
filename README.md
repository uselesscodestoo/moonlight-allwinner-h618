# Moonlight Embedded

## 先选分支 / Choose the branch for your kernel

**按开发板运行的内核与驱动栈选择，不要只看 H618 型号。Choose by the
installed kernel/driver stack, not the SoC name alone.**

| 系统 / Installed system | 使用分支 / Branch | 解码与显示 / Video path |
| --- | --- | --- |
| Armbian current / mainline-family Linux, Cedrus V4L2 Request and Mesa/Panfrost; reference board: Orange Pi Zero 2W | [`h618-egl-download`](https://github.com/uselesscodestoo/moonlight-allwinner-h618/tree/h618-egl-download) | Cedrus → custom libva-v4l2-request → DMA-BUF/EGL → X11 |
| KICKPI K2B vendor Longan Linux 5.4.125, vendor Cedar device and `/dev/disp` | [`k2b-cedarc-disp`](https://github.com/uselesscodestoo/moonlight-allwinner-h618/tree/k2b-cedarc-disp) | CedarC VPU → NV12 DMA-BUF → vendor DE33 `/dev/disp` → HDMI |

These are two different kernel interfaces, not interchangeable launch modes.
A stock kernel alone is not sufficient for the current/mainline route: its
documented companion driver and userspace setup are also required. The vendor
route needs the matching vendor kernel, headers, private CedarC runtime and
`cedar_test_heap` module; it does not require Mali acceleration for video.
Other boards/images with the same H618 are not automatically validated.

`uname -r` helps identify the kernel, but a version string or the existence
of a `/dev/video*` node alone does not prove compatibility. Confirm the image
and actual driver stack against the selected branch's documentation.

**You are reading `h618-egl-download`, the Armbian/current (mainline-family)
kernel route.** For the KICKPI Longan 5.4 vendor image and direct DE33 display,
use [`k2b-cedarc-disp`](https://github.com/uselesscodestoo/moonlight-allwinner-h618/tree/k2b-cedarc-disp).
That branch defaults to HEVC and includes its own build, heap-module and
desktop-restoration instructions. Its HEVC work is already merged; new users
do not need the temporary `k2b-hevc` branch.

The EGL notes below contain historical measurements from different render
paths. See [the detailed experiments](docs/h618-zero-copy.md), including
native NV12 external sampling, before interpreting an older frame-rate
figure as a limit for every path. They are not vendor DE33 measurements.

## Provenance and changes in this repository

This tree is a **derived work** of
[`moonlight-stream/moonlight-embedded`](https://github.com/moonlight-stream/moonlight-embedded)
(GPL-3.0, see `LICENSE`), published as the branch **`h618-egl-download`**.  This
is a GitHub fork of the upstream repository, maintained independently: the
changes target the Allwinner H618 and are not intended for upstream.  All
upstream credit belongs to the Moonlight Embedded authors.

* Upstream: <https://github.com/moonlight-stream/moonlight-embedded>
* Base commit of this tree: `f32e415` ("libgamestream: fix uniqueid.dat read check")
* Full history: the tree started as a `git clone --depth 1` and has since been
  unshallowed, so the complete upstream history up to `f32e415` is present.
* Companion driver: [`bootlin/libva-v4l2-request`](https://github.com/bootlin/libva-v4l2-request)
  with the H618 cedrus port and linear-NV12 capture support (branch
  `h618-c-port`).

### What this branch adds

| Commit | Change |
|---|---|
| `d782288` | Render VAAPI frames through `hwdownload` + EGL instead of `vaapi_queue` |
| `7eabd02` | **Zero-copy display**: import the decoder's dma-buf directly - linear NV12 as a native NV12 texture, `SUNXI_TILED_NV12` as an R8 texture de-tiled in the fragment shader |
| `e775aeb` | Keep the output awake while streaming (DPMS/screen saver); `MOONLIGHT_NO_VSYNC` switch |
| `19ab0ae` | Derive the YCbCr->RGB conversion from the stream's own metadata (Sunshine tags limited-range BT.601) |
| `679437a` | GPU self-test for that conversion (`MOONLIGHT_COLOR_SELFTEST=1`) |
| `569c420` | `tools/egl_bench.c`: controlled display-path benchmark |
| `bb929e9` | Force DPMS on when a stream starts; benchmark uses the software path |
| `f9bd685` | Per-10-frame instrumentation: fps, host:client frame ratio, per-stage cost |
| `8f27906` | Optional blanking calls can no longer kill the client (`DPMSForceLevel` with the extension disabled raised BadMatch) |
| `docs/` | `docs/h618-zero-copy.md` - the zero-copy design, the board traps and every debug switch |

See `docs/h618-zero-copy.md` for the design and `scripts/h618-quick-start.sh`
for a reference launch script (relative paths; documented for the Orange Pi
Zero 2W / H618, adapt as needed).

### Building (H618 reference configuration)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2      # binary: build/moonlight
# run (see scripts/h618-quick-start.sh):
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=<libva build or install dir> \
  ./build/moonlight -platform x11_vaapi -codec h265 -1080 -fps 60 -app Desktop stream <host>
```

### Known issues

* A fixed **vertical tear** can appear under heavy motion (e.g. scrolling) on
  the zero-copy path; it does not affect normal use.  Recorded with the
  suspected cause and a way to confirm it in `docs/h618-zero-copy.md`.
* HEVC streams with more than one B frame per group differ from a software
  decoder on the first B frame after each IDR (hardware behaviour; H264 is
  unaffected).  See the companion driver's `docs/zero-copy-results.md`.
* 1080p HEVC zero-copy runs at ~34-36 fps on this board; the limit is the GPU
  sampling the decoder's dma-buf, not the X server or vsync (measured with
  `MOONLIGHT_ZC_BREAKDOWN=1`; see `docs/h618-zero-copy.md`).  The compositor
  costs a further ~10 ms/frame, so keep it off while streaming.

 [![Build](https://img.shields.io/github/actions/workflow/status/moonlight-stream/moonlight-embedded/build.yml?branch=master)](https://github.com/moonlight-stream/moonlight-embedded/actions/workflows/build.yml?query=branch%3Amaster) [Nightly Build Downloads](https://nightly.link/moonlight-stream/moonlight-embedded/workflows/build/master)

Moonlight Embedded is an open source client for [Sunshine](https://github.com/LizardByte/Sunshine) and NVIDIA GameStream for embedded Linux systems, like Raspberry Pi, CuBox-i and ODROID. Moonlight allows you to stream your full collection of games and applications from your PC to other devices to play them remotely.

Moonlight also has [PC](https://github.com/moonlight-stream/moonlight-qt), [Android](https://github.com/moonlight-stream/moonlight-android), and [iOS](https://github.com/moonlight-stream/moonlight-ios) clients.

## Documentation

More information about installing and running Moonlight Embedded is available on the [wiki](https://github.com/moonlight-stream/moonlight-embedded/wiki).

## Bugs

Please check the wiki and old bug reports before submitting a new bug report.

Bugs can be reported to the [issue tracker](https://github.com/moonlight-stream/moonlight-embedded/issues).

## See also

[Moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) is the shared codebase between different Moonlight implementations

## Contribute

1. Fork us
2. Write code
3. Send Pull Requests
