# Moonlight Embedded — H618 vendor-kernel edition

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

## 本分支 / This branch: `k2b-cedarc-disp`

Use this branch for the tested **KICKPI K2B Longan Linux 5.4.125** image.
`k2b-hevc` was the HEVC experiment branch; it has been merged here. New vendor
kernel users should use `k2b-cedarc-disp`, not the experiment branch.

For Armbian/current with Cedrus and EGL, switch to
[`h618-egl-download`](https://github.com/uselesscodestoo/moonlight-allwinner-h618/tree/h618-egl-download).
Do not use that branch's `x11_vaapi` launch instructions on this vendor image.

### Normal use / 日常启动

After the native build, matching heap module setup and Sunshine pairing:

```sh
# From this repository; replace the address with your Sunshine host.
sudo sh tools/k2b-stream.sh <SUNSHINE_IP>
# Optional compatibility/quality comparison:
sudo sh tools/k2b-stream.sh <SUNSHINE_IP> -codec h264
```

Default: **HEVC Main 8-bit SDR, actual 1920×1080 decode/output, 60 fps target,
15 Mbps**, headphone audio through the desktop user's Pulse server. The raw
K2B backend also prefers HEVC for `-codec auto`; explicit H.264 remains supported.
HDR/Main10 and higher frame rates are not enabled.

Exit with **Ctrl+Shift+Alt+Q**. The managed systemd session restores the existing
XFCE framebuffer/HDMI output after exit; a brief black interval is expected.
Do not use SIGKILL on the live decoder to force DMA-buffer teardown. Forced
power loss or a kernel hang cannot be repaired by a userspace post-hook.

### Build, prerequisites and measured status

- [HEVC build/run instructions and evidence](docs/k2b-hevc.md).
- [Board setup, heap-module autoload, audio, recovery and test history](docs/k2b-live-stream.md).
- [Pinned vendor blob hashes](docs/k2b-cedarc-blobs.sha256). Supply the exact
  documented archive and matching kernel headers; do not substitute arbitrary
  CedarX/CedarC releases or install these private libraries globally.

The video path uses the same decoded NV12 DMA-BUF in VPU and DE33, without
software decoding, GPU upload or a decoded-pixel copy. Compressed input is
still copied into Cedar's stream buffer.

Fixed 1080p60 HEVC sample: 600 decoded/display-submitted frames at 59.997
submissions/s; all 600 visible NV12 frames exactly matched the software decode
reference. A 90-second HEVC live invocation completed with progressing output
and successful desktop/resource cleanup. These measurements are not an
end-to-end latency or optical scanout measurement. See the checkpoint for
remaining visual/long-run acceptance limits; earlier AVC gaming acceptance
does not imply a HEVC long-duration pass.

### Provenance

This is a GPL-3.0 derived work of
[moonlight-stream/moonlight-embedded](https://github.com/moonlight-stream/moonlight-embedded)
(see [LICENSE](LICENSE)), developed from this repository's H618 work.
The vendor OS is Longan-derived. The pinned CedarC userspace package's Tina
provenance does not mean this board runs a Tina SDK kernel.
Upstream credit belongs to the Moonlight Embedded authors; these board-specific
changes are maintained independently. The inherited EGL research remains in
the tree, but its historical performance notes do not describe this vendor
`-platform k2b` path.

## Upstream project information

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
