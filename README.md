# Moonlight Embedded

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
* The X server, not the client, limits presentation: ~29 fps at 1080p with
  xfwm4's compositor and ~48-53 fps without it (size-independent).

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
