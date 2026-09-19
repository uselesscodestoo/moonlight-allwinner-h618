# H618 G2D offload: NV12 -> RGB, then DE/KMS scanout

Date: 2026-09-19
Status: draft (pending review)
Worktree / branch: `F:\temp\wt-60fps` @ `h618-60fps`
Scope of first implementation: **Phase 1 only** (kernel driver prototype + standalone proof).

## 1. Why this exists

The zero-copy display work established that the Mali GPU is the wrong engine:

* Sampling the decoder's external NV12 dma-buf costs **~13.5 ms/frame** per-pixel
  (measured: a constant-texel variant removes exactly that), which is close to
  the Mali-G31 texture-unit throughput limit, plus **~8.8 ms** of fixed
  per-texture cost -> ~22 ms -> ~34 fps at 1080p.
* Plan C (copy into GPU-native textures first) was a regression (36 ms).
* The dma-buf's cache attribute is not the issue (panfrost maps every BO
  write-back).
* The display engine cannot scan YUV: DE33 VI-plane YUV needs the DE33 VI
  scaler, whose blocker is RCQ/DMA register shadowing timing; that work is
  unmerged and not small. "1:1 YUV without the scaler" is a false premise
  (chroma still needs 2:1 upsampling).

The H616's **G2D** 2D engine can read NV12 and write RGB. The DE can scan RGB
natively (no VI scaler, no GPU). This is the only mainline-compatible hardware
path that avoids both the Mali per-pixel sampling and the missing DE33 scaler.

## 2. Goals / non-goals

### Goals (Phase 1)

Prove, on the board, that: the decoder's NV12 dma-buf can be converted to RGB by
G2D and scanned by the DE at **1920x1080, 60 fps, near-zero CPU/GPU**, with
correct colours and no tearing.

### Non-goals (Phase 1)

* Moonlight integration, input, audio, session/VT handling (Phase 2).
* Upstream-quality driver/ABI (Phase 3).
* 4K, rotation, scaling beyond 1:1 (the block supports <=2048x2048; keep 1:1).
* Replacing the GPU path in the product; it stays as the fallback.

## 3. Roadmap

1. **Phase 1 (this spec's implementation):** G2D char-driver prototype + a
   standalone test that converts NV12 (synthetic, then decoder) to RGB and
   scans it out via DRM/KMS.
2. **Phase 2:** a DRM/KMS presentation backend in `moonlight-embedded` that uses
   G2D + page flip, with input (evdev/libinput) and session handling.
3. **Phase 3:** rewrite the driver as an upstream-quality **V4L2 mem2mem**
   driver (modelled on `drivers/media/platform/sunxi/sun8i-rotate`), with a
   proper format/selection API and dma-buf fences, for mainline submission.
   Phase 1's register work is reused; only the ABI/framework changes.

## 4. Architecture (Phase 1)

```
cedrus (VPU)  --NV12 dma-buf-->  /dev/g2d (sunxi-g2d.ko)  --XRGB8888-->  DRM fb
                                                                          |
                                                              KMS atomic page flip
                                                                          v
                                                                    DE -> HDMI
```

* **Kernel:** `sunxi-g2d.ko`, an **out-of-tree module** built against the
  board's `linux-headers` (no kernel reflash), plus a **device-tree overlay**
  (Armbian `overlay-user`) that adds the `g2d` node (base `0x01480000`, its
  clocks and IRQ). A character device `/dev/g2d` with one blocking ioctl.
* **Userspace:** a standalone `tools/g2d-kms-test` that allocates an NV12
  buffer, runs G2D into an XRGB8888 DRM buffer, and page-flips it in a loop.
  Synthetic NV12 first, then the real decoder dma-buf.

### 4.1 Kernel driver (prototype ABI)

* Device: platform driver matching the DT node; `ioremap` the register block;
  enable `g2d`, `bus-g2d`, `mbus-g2d` clocks; deassert reset (self-reset via the
  block's `G2D_AHB_RESET` if no CCU binding exists); request IRQ; register a
  misc/char device.
* dma-buf: import src and dst via `dma_buf_attach`/`map_attachment`; use the DMA
  API for any cache maintenance; drop on completion.
* Formats: **NV12 (2-plane) in, XRGB8888/ARGB8888 out**, 1:1, at most
  2048x2048; validate alignment (width/height even; stride requirements).
* ioctl `G2D_IOC_BLIT` (prototype, private ABI):
  `{ src_fd, src_format, src_w, src_h, src_stride0, src_stride1, dst_fd,
     dst_format, dst_w, dst_h, dst_stride }`; blocks until the IRQ completes
  (with a timeout).
* Register sequences ported from the vendor `sunxi_g2d` driver
  (`g2d_bsp_v2.c` + `g2d_regs_v2.h`, tree
  `orangepi-xunlong/linux-orangepi` branch `orange-pi-4.9-sun50iw9`).

### 4.2 Userspace standalone test

`tools/g2d-kms-test`:
1. `drmModeGetResources`, pick the HDMI CRTC and its primary/overlay plane.
2. Allocate the destination XRGB8888 buffer as a DRM framebuffer
   (`drmModeAddFB2`), double-buffered.
3. Source NV12: (a) synthesized in a `/dev/dma_heap/default_cma_region` buffer
   (contiguous, required by the DE/G2D), or (b) the decoder's exported dma-buf
   (from an ffmpeg/libva decode of `handoff/test-streams/`).
4. Loop: `G2D_IOC_BLIT` -> `drmModeAtomicCommit(PAGE_FLIP_EVENT)` -> wait for the
   flip -> next buffer. Print per-second fps and the G2D conversion time.
5. Colour verification: read back one frame and compare a few known YUV triples.

### 4.3 Display path

Primary plane, `DRM_FORMAT_XRGB8888`, `DRM_FORMAT_MOD_LINEAR`, atomic page flip
with double buffering. No X server needed; the test takes the CRTC directly
(run it on a free VT or stop the session first).

## 5. Acceptance criteria (Phase 1)

1. The module loads and the DT overlay applies without a kernel reflash
   (`dmesg` clean, `/dev/g2d` present).
2. G2D converts a 1920x1080 NV12 frame to XRGB8888 correctly (colour check) in
   **< 5 ms**, measured over many frames.
3. The DE scans the result at **60 fps** with **CPU < 5 %** (the conversion is
   hardware; the loop is just ioctl + page flip).
4. No tearing under motion (atomic page flip, one buffer per flip).
5. Then the same with the **decoder's** dma-buf as the G2D source (still 60 fps,
   CPU < 5 %), at 1920x1080 HEVC.
6. Fallbacks/rollback documented: `rmmod`, disable the overlay, remove headers.

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Register sequences not documented | Port from the vendor `sunxi_g2d` source (no reverse-engineering) |
| Board has no `linux-headers` installed | Install the matching `linux-headers-current-sunxi64` package (no kernel swap) |
| DT overlay support on this Armbian build | Verify `overlay-user` works; fallback: rebuild/replace the DTB (still no kernel swap) |
| Clock/reset/IRQ names differ from vendor DT | Read the vendor `sun50iw9p1.dtsi` and the mainline H616 CCU bindings |
| dma-buf cache coherency G2D -> DE | Use the DMA API; test with the hardware; page-flip only after G2D completion |
| G2D and DE bandwidth contention at 60 fps | Measure; 1080p NV12->RGB is ~9 MB/frame, well within DDR bandwidth |
| IOMMU absent for G2D | Use physically contiguous CMA heaps (like the decoder path) |

## 7. Research/unknowns to close before/while implementing

* Armbian DT-overlay support and the exact `linux-headers` package for
  `6.18.51-current-sunxi64`.
* H616 G2D compatible string, IRQ number, clock/reset binding names (from the
  vendor DT).
* The minimal register sequence for NV12 -> XRGB8888 1:1 (from `g2d_bsp_v2.c`).
* Whether G2D requires cache maintenance for the DE to see its output.

## 8. Out of scope (Phase 1)

Moonlight, input, audio, X integration, 4K, scaling/rotation, upstream
submission. Recording the GPU zero-copy path as the fallback.
