# H618 G2D offload — Phase 1 implementation plan

> **Status (2026-09-20): abandoned after Task 3.** Tasks 1–3 done and committed;
> the G2D **sub-block registers never come alive** on a mainline boot and no
> public/vendor source reveals the missing init. See the spec's "Outcome"
> (`../specs/2026-09-19-h618-g2d-offload-design.md`). Task 4 was not committed.

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** On the Orange Pi Zero 2W (H616), prove that G2D can convert a 1920x1080 NV12 buffer (synthetic first, then the decoder's dma-buf) to XRGB8888 and that the display engine scans it at 60 fps with near-zero CPU.

**Architecture:** an out-of-tree kernel module `sunxi-g2d-h616.ko` plus a device-tree overlay adding the `g2d` node, exposing `/dev/g2d`; a user-space `tools/g2d-kms-test` allocates NV12 (dma-heap), calls the driver, and page-flips the RGB output over DRM/KMS. No kernel reflash: only `linux-headers` + a module + an overlay.

**Tech stack:** Linux 6.18 (Armbian), C, module/DT, DRM/KMS, dma-buf/dma-heap, EGL not involved.

**Spec:** `docs/superpowers/specs/2026-09-19-h618-g2d-offload-design.md`
**Register reference (read this first):** `docs/superpowers/notes/g2d-vendor-extraction.md` — every offset/value below comes from it.

---

## Fixed facts (from the extraction)

* DT: `compatible = "allwinner,sunxi-g2d"`, `reg = <0 0x01480000 0 0x3ffff>`, `interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_HIGH>`, one clock `g2d` (CCU, 300 MHz, parent PLL_PERIPH0x2), no reset line (in-block `G2D_AHB_RESET`).
* Blocks (offsets from base): TOP `0x00000`, MIXER `0x00100`, BLD `0x00400`, V0 `0x00800`, UI0 `0x01000`, WB `0x03000`, VSU `0x08000`.
* Blit path: `G2D_CMD_BITBLT_H`, `flag_h = 0` → **V0 (YUV src) → BLD (+CSC2) → WB (RGB dst)**. Legacy equal-size path silently uses ROT (no YUV→RGB) — do not use it.
* Formats: NV12 semi-planar = `0x28` (or `0x29` if UV order is swapped), XRGB8888 = `0x04`.
* Constants: `0x0437077F = (1080-1)<<16 | (1920-1)`; `0x021B03BF = (540-1)<<16 | (960-1)`; luma/UV pitch `1920`; XRGB pitch `7680`.
* Completion: `G2D_MIXER_INT` (base+`0x104`) enable `0x10`, status bit 0, ack = write the read value back; start = `G2D_MIXER_CTL` (base+`0x100`) bit 31.
* Addresses are 40-bit (low32 + high8). With no IOMMU, program **physical** addresses (`use_phy_addr` style).

## Preconditions / constraints

* Board `scy@172.30.71.3` (key auth; `sudo -n` works). Worktree `F:\temp\wt-60fps` (branch `h618-60fps`). Shell PowerShell.
* **Do not reflash the kernel.** Everything here is a module + overlay; rollback is `rmmod` + disable the overlay.
* The userspace test takes the CRTC directly (no X). Stop any moonlight/X use for its run; the screen is otherwise free (`pgrep -a moonlight` = none, VT7 active).
* Never edit `/home/scy/Downloads/moonlight-embedded`.

---

## Task 1: Board prep — headers, overlay support, build dir

**Files:** none (infra).

- [ ] **Step 1: Confirm the matching headers are installable and install them**

```sh
ssh scy@172.30.71.3 'apt-cache policy linux-headers-current-sunxi64 2>/dev/null | head -4; uname -r'
```
If a matching version (`6.18.51-current-sunxi64`) is available:
```sh
ssh scy@172.30.71.3 'sudo apt-get install -y linux-headers-current-sunxi64'
ssh scy@172.30.71.3 'ls -d /lib/modules/$(uname -r)/build && echo HEADERS_OK'
```
Expected: `HEADERS_OK`.

- [ ] **Step 2: Confirm device-tree overlay support**

```sh
ssh scy@172.30.71.3 'ls -d /boot/overlay-user 2>/dev/null; grep -i overlay /boot/armbianEnv.txt 2>/dev/null; ls /boot/dtb/allwinner/overlay 2>/dev/null | head'
```
Record whether `/boot/overlay-user/` exists and whether `armbianEnv.txt` has an `user_overlays=`/`overlays=` line. If overlays are unsupported, the fallback is to append the node to the board DTB and reinstall `linux-dtb` (still no kernel reflash) — note it and continue; Task 2 provides the `.dts` either way.

- [ ] **Step 3: Note the reference tree**

The vendor G2D source is already summarised in `docs/superpowers/notes/g2d-vendor-extraction.md`; fetch the two headers into the worktree for offline reference (optional):
```
mkdir -p F:\temp\wt-60fps\kernel\reference
# fetch g2d_regs_v2.h and g2d_bsp_v2.c from
# https://raw.githubusercontent.com/orangepi-xunlong/linux-orangepi/orange-pi-4.9-sun50iw9/drivers/char/sunxi_g2d/
```
No commit.

---

## Task 2: Module scaffold + DT overlay + skeleton driver

**Files:**
- Create: `kernel/g2d/Makefile`
- Create: `kernel/g2d/g2d_regs.h`
- Create: `kernel/g2d/g2d_h616.c`
- Create: `kernel/g2d/sunxi-g2d-h616-overlay.dts`

- [ ] **Step 1: `kernel/g2d/Makefile`**

```makefile
obj-m += sunxi-g2d-h616.o
sunxi-g2d-h616-y := g2d_h616.o
```

- [ ] **Step 2: `kernel/g2d/g2d_regs.h`** — offsets + constants from the extraction

```c
#ifndef G2D_REGS_H
#define G2D_REGS_H

#define G2D_TOP         0x00000
#define G2D_MIXER       0x00100
#define G2D_BLD         0x00400
#define G2D_V0          0x00800
#define G2D_UI0         0x01000
#define G2D_WB          0x03000
#define G2D_VSU         0x08000

/* TOP */
#define G2D_SCLK_GATE   (G2D_TOP + 0x00)
#define G2D_HCLK_GATE   (G2D_TOP + 0x04)
#define G2D_AHB_RESET   (G2D_TOP + 0x08)
#define G2D_SCLK_DIV    (G2D_TOP + 0x0C)
/* MIXER */
#define G2D_MIXER_CTL   (G2D_MIXER + 0x00)   /* bit31 = START */
#define G2D_MIXER_INT   (G2D_MIXER + 0x04)   /* en bit4, status bit0, ack write-back */
/* BLD */
#define BLD_EN_CTL      (G2D_BLD + 0x00)
#define BLD_CH_ISIZE0   (G2D_BLD + 0x20)
#define BLD_CH_OFFSET0  (G2D_BLD + 0x30)
#define BLD_SIZE        (G2D_BLD + 0x48)
#define BLD_OUT_COLOR   (G2D_BLD + 0x60)
#define ROP_CTL         (G2D_BLD + 0x80)
#define BLD_CSC_CTL     (G2D_BLD + 0x100)    /* bit2 = CSC2 */
#define BLD_CSC2_COEF0  (G2D_BLD + 0x170)
/* V0 */
#define V0_ATTCTL       (G2D_V0 + 0x00)
#define V0_MBSIZE       (G2D_V0 + 0x04)
#define V0_COOR         (G2D_V0 + 0x08)
#define V0_PITCH0       (G2D_V0 + 0x0C)
#define V0_PITCH1       (G2D_V0 + 0x10)
#define V0_PITCH2       (G2D_V0 + 0x14)
#define V0_LADD0        (G2D_V0 + 0x18)
#define V0_LADD1        (G2D_V0 + 0x1C)
#define V0_LADD2        (G2D_V0 + 0x20)
#define V0_HADD         (G2D_V0 + 0x28)
#define V0_SIZE         (G2D_V0 + 0x2C)
/* WB */
#define WB_ATT          (G2D_WB + 0x00)
#define WB_SIZE         (G2D_WB + 0x04)
#define WB_PITCH0       (G2D_WB + 0x08)
#define WB_PITCH1       (G2D_WB + 0x0C)
#define WB_LADD0        (G2D_WB + 0x14)
#define WB_HADD0        (G2D_WB + 0x18)
/* VSU */
#define VS_CTRL         (G2D_VSU + 0x00)
#define VS_OUT_SIZE     (G2D_VSU + 0x40)
#define VS_GLB_ALPHA    (G2D_VSU + 0x44)
#define VS_Y_SIZE       (G2D_VSU + 0x80)
#define VS_Y_HSTEP      (G2D_VSU + 0x88)
#define VS_Y_VSTEP      (G2D_VSU + 0x8C)
#define VS_Y_HPHASE     (G2D_VSU + 0x90)
#define VS_Y_VPHASE0    (G2D_VSU + 0x98)
#define VS_C_SIZE       (G2D_VSU + 0xC0)
#define VS_C_HSTEP      (G2D_VSU + 0xC8)
#define VS_C_VSTEP      (G2D_VSU + 0xCC)
#define VS_C_HPHASE     (G2D_VSU + 0xD0)
#define VS_C_VPHASE0    (G2D_VSU + 0xD8)
#define VS_Y_HCOEF0     (G2D_VSU + 0x200)
#define VS_Y_VCOEF0     (G2D_VSU + 0x300)
#define VS_C_HCOEF0     (G2D_VSU + 0x400)

#define G2D_FMT_NV12      0x28
#define G2D_FMT_NV21_ALT  0x29
#define G2D_FMT_XRGB8888  0x04
#endif
```

- [ ] **Step 3: `kernel/g2d/sunxi-g2d-h616-overlay.dts`** (adapt the clock phandle/indices on the board; CLK_G2D index from `include/dt-bindings/clock/sun50i-h616-ccu.h`)

```dts
/dts-v1/;
/plugin/;
/ {
    compatible = "allwinner,sun50i-h616";
    fragment@0 {
        target-path = "/soc";
        __overlay__ {
            g2d: g2d@1480000 {
                compatible = "allwinner,sunxi-g2d";
                reg = <0x0 0x01480000 0x0 0x3ffff>;
                interrupts = <0 90 4>;
                clocks = <&ccu 33>;        /* CLK_G2D: verify index */
                clock-names = "g2d";
                status = "okay";
            };
        };
    };
};
```
Build: `dtc -@ -I dts -O dtb -o sunxi-g2d-h616.dtbo sunxi-g2d-h616-overlay.dts`.

- [ ] **Step 4: `kernel/g2d/g2d_h616.c` skeleton** — probe, clocks, reset, `/dev/g2d`, file ops

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "g2d_regs.h"

struct g2d_dev {
    struct device *dev;
    void __iomem *base;
    struct clk *clk;
    int irq;
    struct completion done;
    struct mutex lock;
};

static struct g2d_dev *g2d;

static inline u32 g2d_rd(struct g2d_dev *g, u32 off) { return readl(g->base + off); }
static inline void g2d_wr(struct g2d_dev *g, u32 off, u32 v) { writel(v, g->base + off); }

static void g2d_hw_init(struct g2d_dev *g)
{
    g2d_wr(g, G2D_AHB_RESET, 0x0);
    g2d_wr(g, G2D_AHB_RESET, 0x3);
    g2d_wr(g, G2D_SCLK_GATE, 0x3);
    g2d_wr(g, G2D_HCLK_GATE, 0x3);
}

static irqreturn_t g2d_irq(int irq, void *data)
{
    struct g2d_dev *g = data;
    u32 tmp = g2d_rd(g, G2D_MIXER_INT);
    if (tmp & 0x1) {
        g2d_wr(g, G2D_MIXER_INT, tmp);   /* ack by write-back */
        complete(&g->done);
        return IRQ_HANDLED;
    }
    return IRQ_NONE;
}

static int g2d_open(struct inode *inode, struct file *filp) { return 0; }
static int g2d_release(struct inode *inode, struct file *filp) { return 0; }

static long g2d_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    /* filled in Task 3/4 */
    return -ENOTTY;
}

static const struct file_operations g2d_fops = {
    .owner = THIS_MODULE, .open = g2d_open, .release = g2d_release,
    .unlocked_ioctl = g2d_ioctl,
};

static struct miscdevice g2d_misc = { .minor = MISC_DYNAMIC_MINOR, .name = "g2d", .fops = &g2d_fops };

static int g2d_probe(struct platform_device *pdev)
{
    struct g2d_dev *g;
    int ret;

    g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
    if (!g) return -ENOMEM;
    g->dev = &pdev->dev;
    g->base = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(g->base)) return PTR_ERR(g->base);
    g->clk = devm_clk_get(&pdev->dev, "g2d");
    if (IS_ERR(g->clk)) return dev_err_probe(&pdev->dev, PTR_ERR(g->clk), "clk\n");
    ret = clk_prepare_enable(g->clk);
    if (ret) return ret;
    g->irq = platform_get_irq(pdev, 0);
    if (g->irq < 0) { clk_disable_unprepare(g->clk); return g->irq; }
    ret = devm_request_irq(&pdev->dev, g->irq, g2d_irq, IRQF_TRIGGER_HIGH, "g2d", g);
    if (ret) { clk_disable_unprepare(g->clk); return ret; }
    mutex_init(&g->lock);
    init_completion(&g->done);
    g2d_hw_init(g);
    ret = misc_register(&g2d_misc);
    if (ret) { clk_disable_unprepare(g->clk); return ret; }
    g2d = g;
    dev_info(&pdev->dev, "g2d ready\n");
    return 0;
}

static int g2d_remove(struct platform_device *pdev)
{
    misc_deregister(&g2d_misc);
    return 0;
}

static const struct of_device_id g2d_of[] = { { .compatible = "allwinner,sunxi-g2d" }, {} };
MODULE_DEVICE_TABLE(of, g2d_of);
static struct platform_driver g2d_driver = {
    .probe = g2d_probe, .remove = g2d_remove,
    .driver = { .name = "sunxi-g2d-h616", .of_match_table = g2d_of },
};
module_platform_driver(g2d_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Allwinner H616 G2D (prototype)");
```

- [ ] **Step 5: Cross-build the module (on this Windows host via WSL2, or on the board) and load it**

Recommended: build on the **board** (headers are there; the module is tiny):
```sh
scp -r F:/temp/wt-60fps/kernel/g2d scy@172.30.71.3:~/g2d-mod
ssh scy@172.30.71.3 'cd ~/g2d-mod && make -C /lib/modules/$(uname -r)/build M=$PWD modules'
```
Install the overlay (if supported):
```sh
scp F:/temp/wt-60fps/kernel/g2d/sunxi-g2d-h616.dtbo scy@172.30.71.3:~/   # after building the dtbo
ssh scy@172.30.71.3 'sudo cp ~/sunxi-g2d-h616.dtbo /boot/overlay-user/ 2>/dev/null; grep -q user_overlays /boot/armbianEnv.txt || echo "user_overlays=sunxi-g2d-h616" | sudo tee -a /boot/armbianEnv.txt'
```
(If overlays are unsupported, integrate the node into the DTB and reinstall `linux-dtb` instead.)
Reboot, then:
```sh
ssh scy@172.30.71.3 'sudo insmod ~/g2d-mod/sunxi-g2d-h616.ko && dmesg | tail -5; ls -l /dev/g2d'
```
Expected: `g2d ready`, `/dev/g2d` present. If the DT node is missing, `probe` won't run — fix the overlay/DT.

- [ ] **Step 6: Commit**

```bash
git -C F:\temp\wt-60fps add kernel/g2d
git -C F:\temp\wt-60fps commit -m "kernel: H616 G2D prototype module skeleton + DT overlay"
```

---

## Task 3: dma-buf import, ioctl ABI, register read/write self-test

**Files:** Modify `kernel/g2d/g2d_h616.c`.

- [ ] **Step 1: define the prototype ioctl** (private ABI; keep it minimal)

```c
struct g2d_blt_req {
    __s32 src_fd;        /* NV12 dma-buf (contiguous/CMA) */
    __s32 dst_fd;        /* XRGB8888 dma-buf */
    __u32 width, height; /* e.g. 1920, 1080 */
    __u32 src_format;    /* G2D_FMT_NV12 (0x28) */
    __u32 dst_format;    /* G2D_FMT_XRGB8888 (0x04) */
    __u32 timeout_ms;
};
#define G2D_IOC_BLT  _IOWR('G', 0x55, struct g2d_blt_req)
```

- [ ] **Step 2: import both dma-bufs** (`dma_buf_get` + `dma_buf_attach` + `dma_buf_map_attachment(DMA_BIDIRECTIONAL)`), take `sg_dma_address(sgt->sgl)` as the base physical/IOVA, and record the sizes to compute plane offsets. Reject non-contiguous buffers (`for_each_sg` span check), matching what cedrus does.

- [ ] **Step 3: prove poking works** — a debug branch (`arg == 0`) that writes/reads `G2D_SCLK_GATE` via ioctl to confirm the mapping is live. Verify from userspace with a tiny `ioctl(fd, G2D_IOC_BLT, ...)`.

- [ ] **Step 4: build, load, test, commit**

```sh
ssh scy@172.30.71.3 'cd ~/g2d-mod && make -C /lib/modules/$(uname -r)/build M=$PWD modules && sudo rmmod sunxi-g2d-h616; sudo insmod sunxi-g2d-h616.ko && dmesg | tail -3'
```
Commit: `kernel: G2D dma-buf import and prototype ioctl`.

---

## Task 4: the NV12 -> XRGB8888 blit + IRQ completion

**Files:** Modify `kernel/g2d/g2d_h616.c`.

- [ ] **Step 1: implement `g2d_blit()` exactly per `g2d-vendor-extraction.md` §3.3**

Use the ordered writes (verbatim from the notes): reset pulse; V0 attributes/size/pitches/addresses; VSU pass-through (`0x08000=0x10101`→`0x10001`, steps `0x00100000`, the 32-entry FIR coefficient tables, chroma phase `0xFFFC0000`); BLD/ROP; CSC2 enable + `Ycbcr2rgb_709` coefficients; WB; enable interrupt `0x10`; start `0x80000000`; then `wait_for_completion_timeout(&done, timeout)`. On timeout, `g2d_hw_init()` again and report `-ETIMEDOUT`.

- [ ] **Step 2: test with a synthetic NV12** — add a temporary userspace helper (or a module `arg==0xfeed` branch) that:
  * allocates a `default_cma_region` NV12 buffer and fills Y with a gradient and known UV triples;
  * runs `G2D_IOC_BLT` into an XRGB buffer;
  * reads back a few pixels and compares to the expected RGB (BT.709, since w>1280 → 709 coefficients are used).
  Verify `0x28` gives correct colours; if U/V look swapped, switch to `0x29`.

- [ ] **Step 3: commit** `kernel: G2D NV12 -> XRGB8888 blit with IRQ completion`.

---

## Task 5: standalone DRM/KMS test tool

**Files:** Create `tools/g2d-kms-test.c`, add to `tools/` build (or compile standalone with `pkg-config libdrm`).

- [ ] **Step 1: implement** `drmModeGetResources` → pick the connected connector + CRTC + primary plane; allocate two XRGB8888 dumb buffers + FBs; allocate an NV12 CMA buffer (dma-heap `default_cma_region`); fill it once.
- [ ] **Step 2: loop** `ioctl(G2D_IOC_BLT)` → `drmModeAtomicCommit` with `DRM_MODE_PAGE_FLIP_EVENT` → wait for `DRM_EVENT_FLIP_COMPLETE` → alternate buffers; print fps and the G2D conversion time every second.
- [ ] **Step 3: measure**

```sh
ssh scy@172.30.71.3 'sudo ./g2d-kms-test 1920 1080'   # X/lightdm stopped or on a free VT
```
Expected: **60 fps, CPU near 0**, correct colours, no tearing. Commit.

---

## Task 6: real decoder dma-buf + acceptance

**Files:** Modify `tools/g2d-kms-test.c` (add a mode) or add `tools/g2d-dec-test.c`.

- [ ] **Step 1:** decode a 1080p HEVC stream with `ffmpeg`/libva using the custom driver (`LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=~/Downloads/v4l2-dri`), export each decoded frame's dma-buf (`vaExportSurfaceHandle`), hand it to `G2D_IOC_BLT`, and page-flip the RGB result.
- [ ] **Step 2:** run over `handoff/test-streams/t1080.hevc` (or a live 60 fps source); record fps/CPU and colour.
- [ ] **Step 3: commit** and write the results into the spec.

---

## Task 7: document and close Phase 1

- [ ] Update `docs/superpowers/specs/2026-09-19-h618-g2d-offload-design.md` with the measured results, the verified NV12 enum, and any corrections.
- [ ] Add a short `kernel/g2d/README.md`: build/load/overlay/rollback commands.
- [ ] Commit.

---

## Open questions carried into execution

1. Real H616 CCU clock index/phandle for `clk_g2d` in the **mainline** DT (the vendor used a `periph-clock` node; mainline uses `&ccu CLK_G2D`). Resolve in Task 2.
2. Overlay support on this Armbian build; else DTB path. (Task 1)
3. NV12 vs NV21 enum (`0x28`/`0x29`) — resolve empirically in Task 4.
4. Whether G2D output needs cache maintenance for the DE (DMA API handles it; verify visually).
