# Allwinner H616 / H618 G2D vendor extraction — NV12 → XRGB8888 1:1

Source tree studied (public vendor kernel):
`github.com/orangepi-xunlong/linux-orangepi`, branch **`orange-pi-4.9-sun50iw9`**.

Files read (all fetched raw from that branch):
- `drivers/char/sunxi_g2d/g2d_regs_v2.h`
- `drivers/char/sunxi_g2d/g2d_bsp_v2.c`
- `drivers/char/sunxi_g2d/g2d_bsp.h`
- `drivers/char/sunxi_g2d/g2d_driver.c`
- `drivers/char/sunxi_g2d/g2d_driver_i.h`
- `include/linux/g2d_driver.h`  (public UAPI-ish header, `g2d_fmt_enh`, structs, ioctls)
- `arch/arm64/boot/dts/sunxi/sun50iw9p1.dtsi` (generic sun50iw9p1)
- `arch/arm64/boot/dts/sunxi/sun50iw9p1-clk.dtsi`
- `arch/arm64/boot/dts/sunxi/sun50i-h616-orangepi-zero2.dts` (board DT; H616 Zero2 — the Zero2W shares this SoC node set)

Conventions in this document:
- All offsets are **relative to the G2D register base** (`0x01480000`). `write32(base + OFFSET, value)`.
- Values are copied verbatim from the vendor headers/code.
- Inferences that are not directly established by the source are marked **[SPECULATION]**.

---

## 1. Device description (DT node)

### 1.1 Generic sun50iw9p1 DT — `sun50iw9p1.dtsi` line 1379

```dts
g2d: g2d@01480000 {
        compatible = "allwinner,sunxi-g2d";
        reg = <0x0 0x01480000 0x0 0x3ffff>;
        interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_HIGH>;
        clocks = <&clk_g2d>;
        iommus = <&mmu_aw 6 1>;
};
```

### 1.2 Board DT compiled form — `sun50i-h616-orangepi-zero2.dts` line 3539

(The published board DTB is a flattened/`dtc`-decoded variant; phandles expanded.)

```dts
g2d@01480000 {
        compatible = "allwinner,sunxi-g2d";
        reg = <0x0 0x1480000 0x0 0x3ffff>;      /* 0x01480000, size 256 KiB  */
        interrupts = <0x0 0x5a 0x4>;            /* GIC_SPI 90, LEVEL_HIGH    */
        clocks = <0xd>;                          /* &clk_g2d                  */
        iommus = <0x28 0x6 0x1>;                 /* &mmu_aw, master id 6, 1   */
        /* no status property -> enabled (kernel default) */
};
```

Clock node (`sun50i-h616-orangepi-zero2.dts` line 425, identical to
`sun50iw9p1-clk.dtsi` line 301):

```dts
g2d {
        #clock-cells = <0x0>;
        compatible = "allwinner,periph-clock";
        assigned-clock-parents = <0xc>;          /* pll_periph0x2 (see below) */
        assigned-clock-rates  = <0x11e1a300>;    /* 300,000,000 Hz            */
        assigned-clocks       = <0xd>;
        clock-output-names    = "g2d";
        linux,phandle = <0xd>;
};
```

Facts:
| property | value |
|---|---|
| compatible | `allwinner,sunxi-g2d` |
| reg base | `0x01480000` |
| reg size | `0x3ffff` → 256 KiB (`0x40000`) |
| interrupts | `GIC_SPI 90`, `IRQ_TYPE_LEVEL_HIGH` (Linux virq = 32 + 90 = **122**) |
| clocks | 1 clock, `clk_g2d`, output-name `"g2d"`, periph-clock (CCU) |
| typical clock rate | **300 MHz**, parent `pll_periph0x2` (`clk_pll_periph0x2`) |
| resets | **none in DT** — reset is a module-internal register (`G2D_AHB_RESET`) |
| iommus | `mmu_aw` master id **6** (Allwinner IOMMU). Second cell `1` meaning not established from source. |
| clock-frequency / assigned-clock-rates property on node | not present; rate comes from the CCU clock node (300 MHz) |

Notes:
- The driver `g2d_probe()` (`g2d_driver.c:1566`) does `of_iomap(node,0)`,
  `irq_of_parse_and_map(node,0)`, `request_irq(...)`, `of_clk_get(node,0)` and
  stores the **parent** clock (`clk_get_parent`) to re-use on open/resume.
- There is **no pinctrl** for G2D and **no explicit reset/clock gate DT node**;
  the block is brought up purely by the CCU clock + the in-block TOP registers
  (section 6).
- The driver sets a 32-bit DMA mask: `sunxi_g2d_dma_mask = DMA_BIT_MASK(32)`
  (`g2d_driver.c:1565`, `g2d_probe`). So all programmed physical/IOVA addresses
  fit in 32 bits **provided the IOMMU is in use or memory is below 4 GiB**.

---

## 2. Register map needed for a blit

From `g2d_regs_v2.h` — module base offsets (relative to `0x01480000`):

| block | offset |
|---|---|
| TOP | `0x00000` |
| MIXER | `0x00100` |
| BLD | `0x00400` |
| V0 (video/YUV channel 0) | `0x00800` |
| UI0 | `0x01000` |
| UI1 | `0x01800` |
| UI2 | `0x02000` |
| WB (write-back) | `0x03000` |
| VSU (scaler) | `0x08000` |
| ROT | `0x28000` |
| GSU | `0x30000` |

### TOP (mapped `0x00000`) — clock gates / reset / divider
| name | abs offset | macro |
|---|---|---|
| `G2D_SCLK_GATE` | `0x00000` | `0x00 + G2D_TOP` |
| `G2D_HCLK_GATE` | `0x00004` | `0x04 + G2D_TOP` |
| `G2D_AHB_RESET` | `0x00008` | `0x08 + G2D_TOP` |
| `G2D_SCLK_DIV`  | `0x0000C` | `0x0C + G2D_TOP` |

### MIXER (`0x00100`)
| name | abs offset |
|---|---|
| `G2D_MIXER_CTL` | `0x00100` (START = bit31) |
| `G2D_MIXER_INT` | `0x00104` (enable bit4; status bit0) |
| `G2D_MIXER_CLK` | `0x00108` (clock counter) |

### BLD (`0x00400`)
| name | abs offset |
|---|---|
| `BLD_EN_CTL` | `0x00400` |
| `BLD_FILLC0` | `0x00410` |
| `BLD_FILLC1` | `0x00414` |
| `BLD_CH_ISIZE0` | `0x00420` |
| `BLD_CH_ISIZE1` | `0x00424` |
| `BLD_CH_OFFSET0` | `0x00430` |
| `BLD_CH_OFFSET1` | `0x00434` |
| `BLD_PREMUL_CTL` | `0x00440` |
| `BLD_BK_COLOR` | `0x00444` |
| `BLD_SIZE` | `0x00448` |
| `BLD_CTL` | `0x0044C` |
| `BLD_KEY_CTL` | `0x00450` |
| `BLD_KEY_CON` | `0x00454` |
| `BLD_KEY_MAX` | `0x00458` |
| `BLD_KEY_MIN` | `0x0045C` |
| `BLD_OUT_COLOR` | `0x00460` |
| `ROP_CTL` | `0x00480` |
| `ROP_INDEX0` | `0x00484` |
| `ROP_INDEX1` | `0x00488` |
| `BLD_CSC_CTL` | `0x00500` (bit0=CSC0, bit1=CSC1, bit2=CSC2) |
| `BLD_CSC0_COEF00..CONST2` | `0x00510 .. 0x0053C` (12 words) |
| `BLD_CSC1_COEF00..CONST2` | `0x00540 .. 0x0056C` (12 words) |
| `BLD_CSC2_COEF00..CONST2` | `0x00570 .. 0x0059C` (12 words) |

### V0 — video / YUV layer (`0x00800`)
| name | abs offset |
|---|---|
| `V0_ATTCTL` | `0x00800` (attributes, see §3) |
| `V0_MBSIZE` | `0x00804` |
| `V0_COOR` | `0x00808` |
| `V0_PITCH0` | `0x0080C` |
| `V0_PITCH1` | `0x00810` |
| `V0_PITCH2` | `0x00814` |
| `V0_LADD0` | `0x00818` |
| `V0_LADD1` | `0x0081C` |
| `V0_LADD2` | `0x00820` |
| `V0_FILLC` | `0x00824` |
| `V0_HADD` | `0x00828` (packed high bytes, see §4) |
| `V0_SIZE` | `0x0082C` |
| `V0_HDS_CTL0` | `0x00830` |
| `V0_HDS_CTL1` | `0x00834` |
| `V0_VDS_CTL0` | `0x00838` |
| `V0_VDS_CTL1` | `0x0083C` |

### UI0/UI1/UI2 (`0x01000` / `0x01800` / `0x02000`) — RGB layers
Same 8-register layout on each, offsets relative to the UI base:
| name | rel | UI0 abs | UI1 abs | UI2 abs |
|---|---|---|---|---|
| `UIx_ATTR` | `+0x00` | `0x01000` | `0x01800` | `0x02000` |
| `UIx_MBSIZE` | `+0x04` | `0x01004` | `0x01804` | `0x02004` |
| `UIx_COOR` | `+0x08` | `0x01008` | `0x01808` | `0x02008` |
| `UIx_PITCH` | `+0x0C` | `0x0100C` | `0x0180C` | `0x0200C` |
| `UIx_LADD` | `+0x10` | `0x01010` | `0x01810` | `0x02010` |
| `UIx_FILLC` | `+0x14` | `0x01014` | `0x01814` | `0x02014` |
| `UIx_HADD` | `+0x18` | `0x01018` | `0x01818` | `0x02018` |
| `UIx_SIZE` | `+0x1C` | `0x0101C` | `0x0181C` | `0x0201C` |

### WB — write-back / output (`0x03000`)
| name | abs offset |
|---|---|
| `WB_ATT` | `0x03000` (output pixel format) |
| `WB_SIZE` | `0x03004` |
| `WB_PITCH0` | `0x03008` |
| `WB_PITCH1` | `0x0300C` |
| `WB_PITCH2` | `0x03010` |
| `WB_LADD0` | `0x03014` |
| `WB_HADD0` | `0x03018` |
| `WB_LADD1` | `0x0301C` |
| `WB_HADD1` | `0x03020` |
| `WB_LADD2` | `0x03024` |
| `WB_HADD2` | `0x03028` |

### VSU — scaler (`0x08000`)
| name | abs offset |
|---|---|
| `VS_CTRL` | `0x08000` |
| `VS_OUT_SIZE` | `0x08040` |
| `VS_GLB_ALPHA` | `0x08044` |
| `VS_Y_SIZE` | `0x08080` |
| `VS_Y_HSTEP` | `0x08088` |
| `VS_Y_VSTEP` | `0x0808C` |
| `VS_Y_HPHASE` | `0x08090` |
| `VS_Y_VPHASE0` | `0x08098` |
| `VS_C_SIZE` | `0x080C0` |
| `VS_C_HSTEP` | `0x080C8` |
| `VS_C_VSTEP` | `0x080CC` |
| `VS_C_HPHASE` | `0x080D0` |
| `VS_C_VPHASE0` | `0x080D8` |
| `VS_Y_HCOEF0` | `0x08200` (32 × u32) |
| `VS_Y_VCOEF0` | `0x08300` (32 × u32) |
| `VS_C_HCOEF0` | `0x08400` (32 × u32) |

### ROT (`0x28000`) — rotate block
Needed only for the rotate/flip path (`ROT_CTL 0x28000`, `ROT_INT 0x28004`,
`ROT_IFMT 0x28020`, `ROT_ISIZE 0x28024`, `ROT_IPITCH0/1/2 0x28030/34/38`,
`ROT_ILADD0 0x28040` … `ROT_IHADD2 0x28054`, `ROT_OSIZE 0x28084`,
`ROT_OPITCH0/1/2 0x28090/94/98`, `ROT_OLADD0 0x280A0` … `ROT_OHADD2 0x280B4`).
The 1:1 format-convert op does **not** use ROT (see §3.0).

---

## 3. Minimal register sequence: NV12 → XRGB8888, 1920×1080, 1:1

### 3.0 Which vendor path to use (important)

Two entry points exist:

- `G2D_CMD_BITBLT` (legacy `g2d_blt`) → `g2d_blit()` → `mixer_blt()`
  (`g2d_driver.c:603`, `g2d_bsp_v2.c:1800`).
- `G2D_CMD_BITBLT_H` (`g2d_blt_h`) → `g2d_blit_h()` → **directly**
  `g2d_bsp_bitblt()` (`g2d_driver.c:890` / `g2d_driver.c:1029`).

**Trap in `mixer_blt()`**: when `src` and `dst` have equal width and height it
diverts to the ROT block (`g2d_bsp_v2.c:1875-1880`):

```c
case G2D_BLT_NONE:
    if ((dst->width == src->width) && (dst->height == src->height)) {
        result = g2d_bsp_bitblt(src, dst, G2D_ROT_0);   /* 0x400 -> ROT path */
```

Inside `g2d_bsp_bitblt()` the flag `0x400` selects the `flag & 0xff00` (ROT)
branch, and the ROT block **does not do YUV→RGB conversion** (`dst->format =
src->format`, `g2d_bsp_v2.c:2203-2204`). So the legacy equal-size path cannot
convert NV12→RGB.

**Therefore use the `_H` path**: `G2D_CMD_BITBLT_H` with
`flag_h = G2D_BLT_NONE_H = 0x0`. `g2d_blit_h()` calls `g2d_bsp_bitblt()` with
that flag, which satisfies `G2D_BLT_NONE == (flag & 0x0fffffff)`
(`g2d_bsp_v2.c:1966`) and takes the **mixer** branch:
**V0 (YUV source) → BLD (+CSC) → WB (RGB destination)**.

`g2d_blit_h()` also forces `src/dst.bpremul = 0`, `bbuff = 1`, `gamut = BT709`
(`g2d_driver.c:1003-1008`) and maps dma-buf fds to addresses via
`g2d_dma_map()`/`g2d_set_info()` unless `use_phy_addr = 1`.

### 3.1 Format enum values

`g2d_fmt_enh` (from `include/linux/g2d_driver.h`) — this is what `g2d_blt_h`
takes (`g2d_image_enh.format` is used **unconverted** by `g2d_bsp_bitblt`):

```
G2D_FORMAT_ARGB8888          = 0x00
G2D_FORMAT_ABGR8888          = 0x01
G2D_FORMAT_RGBA8888          = 0x02
G2D_FORMAT_BGRA8888          = 0x03
G2D_FORMAT_XRGB8888          = 0x04
G2D_FORMAT_XBGR8888          = 0x05
G2D_FORMAT_RGBX8888          = 0x06
G2D_FORMAT_BGRX8888          = 0x07
...
G2D_FORMAT_YUV422UVC_V1U1V0U0 = 0x24
G2D_FORMAT_YUV422UVC_U1V1U0V0 = 0x25
G2D_FORMAT_YUV422_PLANAR      = 0x26
G2D_FORMAT_YUV420UVC_V1U1V0U0 = 0x28   <-- semi-planar YUV420 (2-plane)
G2D_FORMAT_YUV420UVC_U1V1U0V0 = 0x29   <-- semi-planar YUV420 (2-plane)
G2D_FORMAT_YUV420_PLANAR      = 0x2a   (3-plane, NOT what we want)
```

- **NV12 (2-plane, U then V) → `G2D_FORMAT_YUV420UVC_V1U1V0U0` (0x28) or
  `G2D_FORMAT_YUV420UVC_U1V1U0V0` (0x29).**
- **XRGB8888 → `G2D_FORMAT_XRGB8888` (0x04)**; ARGB8888 → `0x00`.

**NV12 vs NV21 mapping is ambiguous in the vendor code.** In
`g2d_format_trans()` (`g2d_bsp_v2.c:1661-1664`):

```c
case G2D_FMT_PYUV420UVC:
    if (pixel_seq == G2D_SEQ_VUVU)                 /* 0x3 */
        return G2D_FORMAT_YUV420UVC_V1U1V0U0;      /* 0x28 */
    return G2D_FORMAT_YUV420UVC_U1V1U0V0;          /* 0x29 */
```

i.e. `pixel_seq==VUVU` (V-first hint = NV21) is mapped to `_V1U1V0U0` while the
default (UV-first = NV12) maps to `_U1V1U0V0`. But the *name* `V1U1V0U0` reads
as the opposite. **[SPECULATION]** Recommend trying `0x28` first for a
U-then-V NV12 plane and, if colors are swapped, use `0x29`. The two differ only
in the UV interleave order inside `V0_LADD1`; both are 2-plane with
`ycnt=1, ucnt=2` per `g2d_byte_cal()`.

### 3.2 Buffers and geometry used below

- src: NV12 dma-buf, physically contiguous. `width=1920`, `height=1080`,
  `clip_rect={0,0,1920,1080}`, `align[0]=align[1]=align[2]=16` (typical vendor
  userspace; see §4). Y plane at `src_base`; UV plane immediately after at
  `src_base + 1920*1080 = src_base + 0x1FA400`.
- dst: XRGB8888 linear framebuffer, `width=1920`, `height=1080`,
  `clip_rect={0,0,1920,1080}`, stride 1920*4 = 7680.
- No scaling, no rotation, `alpha` irrelevant (mode = 0).

Constants used:
```
(1080-1)<<16 | (1920-1)      = 0x0437077F      /* any size reg, W-1/H-1 */
(540-1)<<16  | (960-1)       = 0x021B03BF      /* chroma size reg         */
1920                          = 0x00000780      /* luma / UV pitch (bytes) */
7680                          = 0x00001E00      /* XRGB pitch (bytes)      */
```

### 3.3 Ordered register writes

Derived from `g2d_bsp_bitblt()` mixer branch (`g2d_bsp_v2.c:1956-2031`) plus
`g2d_vlayer_set()` (`:555`), `g2d_vsu_para_set()` (`:1051`),
`g2d_bldin_set()` (`:1330`), `g2d_bld_cs_set()` (`:1379`),
`g2d_csc_reg_set()` (`:375`), `g2d_wb_set()` (`:700`), and the common tail
`g2d_bsp_v2.c:2286-2296`.

```c
/* --- 0. reset pulse (g2d_bsp_reset, g2d_bsp_v2.c:202) --- */
write32(base + 0x00008, 0x0);            /* G2D_AHB_RESET = 0 */
write32(base + 0x00008, 0x3);            /* G2D_AHB_RESET = 3 (deassert mixer+rot) */

/* --- 1. V0 = YUV source (g2d_vlayer_set(0,src), :577-644) --- */
/* V0_ATTCTL = (alpha&0xff)<<24 | bpremul<<17 | (format<<8) | (mode<<1) | 1
   alpha from userspace, bpremul=0 (forced by g2d_blit_h), mode=0.   */
write32(base + 0x00800, 0x00002801);          /* alpha=0x00; NV12=0x28. If alpha=0xff -> 0xFF002801 */
write32(base + 0x00804, 0x0437077F);          /* V0_MBSIZE : (h-1)<<16 | (w-1)                */
write32(base + 0x0082C, 0x0437077F);          /* V0_SIZE   : same                              */
write32(base + 0x00808, 0x00000000);          /* V0_COOR = 0                                   */
write32(base + 0x0080C, 1920);                /* V0_PITCH0 = align(ycnt*width, align[0]) = 1920*/
write32(base + 0x00810, 1920);                /* V0_PITCH1 = align(ucnt*cw,   align[1]) = 1920*/
write32(base + 0x00814, 0);                   /* V0_PITCH2 (vcnt=0)                            */
write32(base + 0x00818, src_base & 0xffffffff);                 /* V0_LADD0 = Y  base + p0*clip.y + 1*clip.x */
write32(base + 0x0081C, (src_base+0x1FA400) & 0xffffffff);      /* V0_LADD1 = UV base + p1*cy   + 2*cx       */
write32(base + 0x00820, src_laddr2 & 0xffffffff);              /* V0_LADD2 = laddr[2] (unused for UVC)      */
write32(base + 0x00828, 0);                   /* V0_HADD = (a0>>32) | (a1>>32)<<8 | (a2>>32)<<16
                                                 all zero for <4GiB/IOVA                            */

/* --- 2. coarse-scaler config: no-op for 1:1 (g2d_calc_coarse :1230) ---
   inw >= outw<<3 ? false ; inh >= outh<<2 ? false  -> no HDS/VDS writes.   */

/* --- 3. VSU pass-through (g2d_vsu_para_set(fmt=0x28,1920,1080,1920,1080,0xff), :1051) --- */
write32(base + 0x08000, 0x00010101);   /* VS_CTRL first write (fmt>0x23 branch) */
write32(base + 0x08040, 0x0437077F);   /* VS_OUT_SIZE = (out_h-1)<<16 | (out_w-1) */
write32(base + 0x08044, 0x000000FF);   /* VS_GLB_ALPHA = alpha = 0xff */
write32(base + 0x08080, 0x0437077F);   /* VS_Y_SIZE  = (in_h-1)<<16 | (in_w-1) */
write32(base + 0x08088, 0x00100000);   /* VS_Y_HSTEP = ((1920<<19)/1920)<<1 = 0x80000<<1 */
write32(base + 0x0808C, 0x00100000);   /* VS_Y_VSTEP = ((1080<<19)/1080)<<1 = 0x80000<<1 */
for (i = 0; i < 32; i++)
    write32(base + 0x08200 + i*4, lan2coefftab32_full[32 + i]); /* VS_Y_HCOEF0[i]
        offset: g2d_vsu_calc_fir_coef(0x80000)=1*32=32 -> "counter = 1" block */
/* fmt==0x28 -> 420 branch (:1139-1158) */
write32(base + 0x080C0, 0x021B03BF);   /* VS_C_SIZE = (540-1)<<16 | (960-1) */
write32(base + 0x080C8, 0x00080000);   /* VS_C_HSTEP = yhstep = 0x80000 */
write32(base + 0x080CC, 0x00080000);   /* VS_C_VSTEP = yvstep = 0x80000 */
for (i = 0; i < 32; i++)
    write32(base + 0x08400 + i*4, lan2coefftab32_full[32 + i]); /* VS_C_HCOEF0[i]
        g2d_vsu_calc_fir_coef(0x40000)=32 */
for (i = 0; i < 32; i++)
    write32(base + 0x08300 + i*4, lan2coefftab32_full[32 + i]); /* VS_Y_VCOEF0[i] */
/* 420 -> chroma phase init (:1201-1213) */
write32(base + 0x08090, 0x00000000);   /* VS_Y_HPHASE  = 0 */
write32(base + 0x08098, 0x00000000);   /* VS_Y_VPHASE0 = 0 */
write32(base + 0x080D0, 0xFFFC0000);   /* VS_C_HPHASE  = 0xFFFc0000 */
write32(base + 0x080D8, 0xFFFC0000);   /* VS_C_VPHASE0 = 0xFFFc0000 */
write32(base + 0x08000, 0x00010001);   /* VS_CTRL final (fmt>=0x23) (:1221-1222) */

/* --- 4. BLD / ROP (g2d_bsp_v2.c:2000-2015) --- */
write32(base + 0x00480, 0x000000F0);   /* ROP_CTL = 0xF0 (bypass ROP, ch0 pass) */
/* g2d_bldin_set(0, rect0={0,0,1920,1080}, premul=0) */
write32(base + 0x00400, rd(0x00400) | (1<<8));  /* BLD_EN_CTL |= enable input ch0 */
write32(base + 0x00420, 0x0437077F);            /* BLD_CH_ISIZE0 = (h-1)<<16|(w-1) */
write32(base + 0x00430, 0x00000000);            /* BLD_CH_OFFSET0 = (y?y-1)<<16|(x?x-1) */
/* g2d_bld_cs_set(src->format=0x28): format>BGRA1010102 -> set BLD_OUT_COLOR bit1 */
write32(base + 0x00460, rd(0x00460) | (1<<1));
/* src->mode==0 -> no UI2 alpha channel, no g2d_bldin_set(1,...) */

/* --- 5. CSC2 = YUV2RGB (g2d_bsp_v2.c:2023-2029 + g2d_csc_reg_set(2, YUV2RGB_709)) ---
   Condition: src->format > BGRA1010102 && dst->format <= BGRA1010102.
   w>1280 || h>720 -> G2D_YUV2RGB_709 (else _601).                          */
write32(base + 0x00500, rd(0x00500) | (1<<2));  /* BLD_CSC_CTL |= enable CSC2 */
__s32 Ycbcr2rgb_709[12] = {          /* g2d_bsp_v2.c:34-36, verbatim */
    0x04a8, 0x0, 0x072c, 0xFFFC1F7D,
    0x04a8, 0xFFFFFF26, 0xFFFFFDDD, 0x133F8,
    0x04a8, 0x0876, 0, 0xFFFB7AA0 };
for (i = 0; i < 12; i++)
    write32(base + 0x00570 + i*4, Ycbcr2rgb_709[i]);   /* BLD_CSC2_COEF00.. */

/* --- 6. WB = RGB destination (g2d_wb_set(dst), :708-782) --- */
write32(base + 0x03000, 0x00000004);   /* WB_ATT = XRGB8888 (dst->format) */
write32(base + 0x03004, 0x0437077F);   /* WB_SIZE */
write32(base + 0x00448, 0x0437077F);   /* BLD_SIZE (also written here) */
write32(base + 0x00460, rd(0x00460) & 0x00000002);  /* BLD_OUT_COLOR premul clear/preserve bit1 */
write32(base + 0x03008, 7680);         /* WB_PITCH0 = align(ycnt*dst_width, align[0]) = 4*1920 */
write32(base + 0x0300C, 0);            /* WB_PITCH1 (cw=0 for RGB) */
write32(base + 0x03010, 0);            /* WB_PITCH2 */
write32(base + 0x03014, dst_base & 0xffffffff);                       /* WB_LADD0 */
write32(base + 0x03018, (dst_base >> 32) & 0xff);                     /* WB_HADD0 */
write32(base + 0x0301C, (dst_laddr1) & 0xffffffff);                   /* WB_LADD1 (unused) */
write32(base + 0x03020, ((dst_laddr1)>>32) & 0xff);                   /* WB_HADD1 */
write32(base + 0x03024, (dst_laddr2) & 0xffffffff);                   /* WB_LADD2 (unused) */
write32(base + 0x03028, ((dst_laddr2)>>32) & 0xff);                   /* WB_HADD2 */

/* --- 7. cut off the ROP/scan-order tail (g2d_bsp_v2.c:2286) --- */
/* g2d_scan_order_fun(flag & 0xf0000000): for flag 0 -> no write.          */

/* --- 8. enable finish interrupt, then START (g2d_bsp_v2.c:2289-2293) --- */
write32(base + 0x00104, 0x00000010);   /* G2D_MIXER_INT = 0x10 (enable) */
write32(base + 0x00100, rd(0x00100) | 0x80000000);   /* G2D_MIXER_CTL START */

/* --- 9. completion: g2d_wait_cmd_finish() (g2d_driver.c:579) ---
   wait_event_timeout(queue, finish_flag==1, 100 ms).
   IRQ handler (g2d_driver.c:530) does:
       if (mixer_irq_query() == 0) { g2d_mixer_reset(); finish_flag=1; wake_up(); }
   mixer_irq_query() (g2d_bsp_v2.c:246):
       tmp = rd(G2D_MIXER_INT);
       if (tmp & 0x1) { write32(G2D_MIXER_INT, tmp); return 0; }   // write-back clears
       return -1;
   (ROT path would use ROT_INT + rot_irq_query.)                            */
```

### 3.4 What each channel does here

- **YUV source → V0** (`g2d_vlayer_set(0, src)` -> registers `0x800..`).
- **RGB destination → WB** (`g2d_wb_set(dst)` -> registers `0x3000..`); the
  BLD output is written back to memory. UI0/UI1/UI2 are **not used** for a
  plain 1:1 bitblt (they are used only for alpha blending/ROP/mask paths).
- **Colour-space conversion** is done by BLD **CSC2** (enabled via
  `BLD_CSC_CTL` bit2) using the `Ycbcr2rgb_709` (or `_601`) coefficient table.
- **BLD input channel 0** is enabled (`BLD_EN_CTL` bit8) with the destination
  rectangle; ROP is bypassed (`ROP_CTL=0xF0`).
- **VSU** is still run in 1:1 pass-through mode (the vendor always scales a YUV
  source), with steps = 1.0 (`0x80000<<1`) and the "counter = 1" FIR block.
  **[SPECULATION]** It may be possible to bypass VSU for a true 1:1, but the
  vendor code does not; mirror it.

---

## 4. Addressing

- **40-bit addresses: low 32 bits + high 8 bits.**
  - `V0`: `V0_LADD0/1/2` hold bits[31:0]; `V0_HADD` packs all three high bytes:
    `HADD = (addr0>>32)&0xff | ((addr1>>32)&0xff)<<8 | ((addr2>>32)&0xff)<<16`
    (`g2d_bsp_v2.c:642-644`).
  - `UIx`: `UIx_LADD` low + `UIx_HADD = (addr>>32)&0xff`
    (`g2d_bsp_v2.c:693-694`).
  - `WB`: separate `WB_LADD{0,1,2}` + `WB_HADD{0,1,2}`, each high byte 8 bits
    (`g2d_bsp_v2.c:770-781`).
  - `ROT`: separate `ROT_ILADD/IHADD`, `ROT_OLADD/OHADD` per plane.
- Address computation applied by the vendor:
  `addr = laddr[n] + ((u64)haddr[n] << 32) + pitch_n * (clip.y/cy) + byte_per_col_n * (clip.x/cx)`
  (`g2d_bsp_v2.c:630-641`, `767-781`). For the 1:1 case with clip at origin the
  effective addresses are simply `laddr[0]` (Y) and `laddr[0] + Ysize` (UV).
- **Stride/pitch is recomputed in the BSP from `width`, ignoring any
  user-supplied pitch**:
  ```
  pitch0 = cal_align(ycnt * width,    align[0]);
  pitch1 = cal_align(ucnt * cw,       align[1]);   cw = width>>1 for 420
  pitch2 = cal_align(vcnt * cw,       align[2]);
  ```
  with `ycnt/ucnt/vcnt` from `g2d_byte_cal()` (`g2d_bsp_v2.c:459`):
  - NV12 (0x28/0x29): `ycnt=1, ucnt=2, vcnt=0` → Y pitch = align(width),
    UV pitch = align(2*(width/2)) = align(width).
  - XRGB8888 (0x04): `ycnt=4` → pitch = align(4*width).
  `cal_align()` supports align ∈ {0,4,8,16,32,64,128}; unknown → 32
  (`g2d_bsp_v2.c:529-549`). **This is an alignment of the *byte* pitch, applied
  by `align[]` supplied by userspace.** No fixed 16/32 hardware requirement is
  stated in these files. Vendor userspace typically passes 16. **[SPECULATION]**
  Recommend 16-byte alignment for both planes: Y stride 1920, UV stride 1920,
  RGB stride 7680 for the 1920-wide case.
- `g2d_set_info()` (`g2d_driver.c:284`) derives `laddr[1]`/`laddr[2]` for
  dma-buf sources using `G2DALIGN(width, align[0])` and
  `G2DALIGN(u_width*(uvc+1), align[1])`; for NV12 these give Ysize = stride*height,
  UVsize = stride*height/2.
- DMA mask is 32-bit (`g2d_driver.c:1565`), so **high bytes are expected to be 0
  when the IOMMU (master 6) supplies 32-bit IOVAs**, but the registers exist and
  should still be programmed.
- Allocation helper `G2D_BYTE_ALIGN(x)` rounds to 4 KiB (`g2d_driver.c:15`).

---

## 5. Interrupt / status

- IRQ: `GIC_SPI 90` (level high) → Linux virq 122.
- `request_irq(info->irq, g2d_handle_irq, 0, dev_name, NULL)` (`g2d_driver.c:1627`).
- Mixer status/enable register: **`G2D_MIXER_INT` (base + 0x104)**
  - enable: `write32(0x104, 0x10)` (`mixer_irq_enable`, `g2d_bsp_v2.c:277`).
  - status: bit **0** = finish pending.
  - ack: **write-back the read value** (`mixer_irq_query`, `g2d_bsp_v2.c:246`):
    ```c
    tmp = read(G2D_MIXER_INT);
    if (tmp & 0x1) { write(G2D_MIXER_INT, tmp); return 0; }  /* clear */
    ```
- ROT status: `ROT_INT` (base + 0x28004); enable `0x10000`; status bit0.
- Completion pattern: `g2d_wait_cmd_finish()` (`g2d_driver.c:579`) does
  `wait_event_timeout(queue, finish_flag==1, msecs_to_jiffies(100))`. The IRQ
  handler (`g2d_driver.c:530`) calls `mixer_irq_query()`, and on success does
  `g2d_mixer_reset()` then sets `finish_flag=1` and `wake_up()`.
- Timeout path: `g2d_bsp_reset()` (mixer) + warning + forced `finish_flag=1`.
- `g2d_bsp.h` also defines `G2D_FINISH_IRQ (1<<8)` / `G2D_ERROR_IRQ (1<<9)` — these
  belong to the **non-V2** (`mixer_get_irq`) path; for V2 (H616) the bit-0
  `mixer_irq_query` path above is used.

---

## 6. Reset / clock bring-up

Clock tree:
- CCU periph clock `clk_g2d`, output name `"g2d"`, parent `pll_periph0x2`,
  `assigned-clock-rates = <300000000>` (**300 MHz**).
- `g2d_open()` (`g2d_driver.c:486`) on first user: `clk_set_parent(clk, clk_parent)`
  then `clk_prepare_enable(clk)`, then `g2d_bsp_open()`.
- `g2d_release()` (`:507`) on last user: `clk_disable(clk)`, `g2d_bsp_close()`.

In-block TOP registers:
```c
g2d_bsp_open():   write32(0x00000, 0x3);  /* G2D_SCLK_GATE  = 3 (mixer+rot) */
                  write32(0x00004, 0x3);  /* G2D_HCLK_GATE  = 3             */
                  write32(0x00008, 0x3);  /* G2D_AHB_RESET  = 3 (deassert)  */

g2d_bsp_close():  write32(0x00008, 0x0);
                  write32(0x00004, 0x0);
                  write32(0x00000, 0x0);

g2d_bsp_reset():  write32(0x00008, 0x0);  /* assert both resets   */
                  write32(0x00008, 0x3);  /* deassert             */

g2d_mixer_reset(): v = read(0x00008); write(0x00008, v & ~0x1); write(0x00008, v | 0x1); /* bit0=mixer */
g2d_rot_reset():   v = read(0x00008); write(0x00008, v & ~0x2); write(0x00008, v | 0x2); /* bit1=rot  */
```
`G2D_SCLK_DIV` (0x0000C): low nibble = mixer divider, bits[7:4] = rot divider
(`g2d_sclk_div`/`rot_sclk_div`, `g2d_bsp_v2.c:301-321`). No default set by the vendor.

Bring-up order (mirror this):
1. enable CCU `g2d` clock (300 MHz, parent PLL_PERIPH0x2);
2. `G2D_AHB_RESET` pulse (0 then 3);
3. `G2D_SCLK_GATE = 3`, `G2D_HCLK_GATE = 3`;
4. per-operation `g2d_bsp_reset()` before programming a blit;
5. run blit, wait on IRQ;
6. on teardown, `g2d_bsp_close()` (gates to 0) and `clk_disable`.

There are **no DT reset lines / reset controller IDs**; all reset is in
`G2D_AHB_RESET`.

---

## 7. Vendor command interface (to mirror in a private ioctl)

Public structs (`include/linux/g2d_driver.h`):

```c
typedef struct {                 /* 12 bytes */
    __s32 x; __s32 y; __u32 w; __u32 h;
} g2d_rect;

typedef struct {                 /* g2d_image_enh */
    int             bbuff;
    __u32           color;
    g2d_fmt_enh     format;      /* e.g. 0x28 NV12, 0x04 XRGB8888 */
    __u32           laddr[3];    /* low 32 bits per plane */
    __u32           haddr[3];    /* high 8 bits per plane */
    __u32           width;       /* buffer width  in pixels */
    __u32           height;      /* buffer height in pixels */
    __u32           align[3];    /* per-plane pitch alignment (0/4/8/16/32/64/128) */
    g2d_rect        clip_rect;   /* crop/position within the buffer */
    __u32           gamut;       /* G2D_BT601/709/2020 */
    int             bpremul;
    __u8            alpha;       /* plane alpha */
    g2d_alpha_mode_enh mode;
    int             fd;          /* dma-buf fd (when use_phy_addr==0) */
    __u32           use_phy_addr;/* 1: use laddr/haddr directly */
} g2d_image_enh;

typedef struct {                 /* g2d_blt_h */
    g2d_blt_flags_h flag_h;      /* BLT_NONE_H=0 for straight 1:1 convert */
    g2d_image_enh   src_image_h;
    g2d_image_enh   dst_image_h;
} g2d_blt_h;
```

Relevant enums:
```c
G2D_BLT_NONE_H = 0x0; ... G2D_ROT_90=0x100, G2D_ROT_180=0x200,
G2D_ROT_270=0x300, G2D_ROT_0=0x400, G2D_ROT_H=0x1000, G2D_ROT_V=0x2000,
G2D_SM_DTLR_1=0x10000000;

G2D_BLT_NONE=0, G2D_BLT_PIXEL_ALPHA=1, G2D_BLT_PLANE_ALPHA=2,
G2D_BLT_MULTI_ALPHA=4, G2D_BLT_SRC_COLORKEY=8, G2D_BLT_DST_COLORKEY=0x10,
G2D_BLT_FLIP_HORIZONTAL=0x20, ... G2D_BLT_SRC_PREMULTIPLY=0x1000,
G2D_BLT_DST_PREMULTIPLY=0x2000;

G2D_PIXEL_ALPHA=0, G2D_GLOBAL_ALPHA=1, G2D_MIXER_ALPHA=2;

/* ioctls */
#define SUNXI_G2D_IOC_MAGIC 'G'
G2D_CMD_BITBLT    = 0x50   (legacy, g2d_blt)
G2D_CMD_FILLRECT  = 0x51
G2D_CMD_STRETCHBLT= 0x52
G2D_CMD_BITBLT_H  = 0x55   -> g2d_blt_h   (use this)
G2D_CMD_FILLRECT_H= 0x56
G2D_CMD_BLD_H     = 0x57
G2D_CMD_MASK_H    = 0x58
G2D_CMD_MEM_REQUEST/RELEASE/GETADR/SELIDX ...
```

Device node: character device `"g2d"` (class `g2d`, `device_create(..., "g2d")`,
`alloc_chrdev_region`), so `/dev/g2d`. Operations are copy_from_user of the
whole struct then call `g2d_blit_h()` etc. (`g2d_ioctl`, `g2d_driver.c:1379`).

For a private ioctl mirror: 5 logical fields suffice — `{format, laddr/haddr or
fd, width, height, align[3], clip_rect, alpha/mode, flag}`. The simplest
NV12→XRGB call is: `flag_h = 0`, `src.format = 0x28`, `dst.format = 0x04`,
both `clip_rect = full frame`, `use_phy_addr = 1` with `laddr[0] = src phys`,
`laddr[1] = src phys + Ysize`, `laddr[2] = 0`, dst `laddr[0] = fb phys`.

---

## 8. Gaps / blockers / uncertainties

1. **NV12 vs NV21 enum ambiguity** (0x28 vs 0x29) — see §3.1. Must be validated
   on hardware; colors/UV swap will tell you. [SPECULATION] on naming.
2. **Exact hardware alignment requirement** for NV12/RGB is **not stated** in
   these vendor files; pitch is `align(ycnt*width, align[n])` and the alignment
   is user-supplied. No evidence of a mandatory 16/32/64 multiple in the source.
3. **BLD routing semantics** (why `g2d_bldin_set(0, dst_rect)` while the source
   is V0, and why `g2d_bld_cs_set(src->format)` sets the YUV bit) are not
   documented; the sequence above is a verbatim mirror of the vendor code.
4. **`iommus = <&mmu_aw 6 1>`** second cell meaning (`1`) not established here.
   If IOMMU is enabled, the programmed addresses must be IOVAs from
   `dma_buf_map_attachment()`; if disabled, use physical addresses with
   `use_phy_addr = 1`.
5. **`G2D_SCLK_DIV`** default is not programmed by the vendor; depends on the
   CCU-assigned 300 MHz. Do not assume a divider.
6. **VSU is not bypassed** for 1:1 in the vendor path; a "minimal" hand-rolled
   driver could skip the 96 FIR-coefficient writes, but that is
   **[SPECULATION]** and untested against hardware.
7. The board DT used is `sun50i-h616-orangepi-zero2.dts` (H616 Zero2). The
   Zero2W is H618/H616 and uses the same `g2d@01480000` node; no Zero2W-specific
   DT file was located in this branch's `sunxi/` directory (only
   `sun50i-h616-orangepi-zero2.dts`).
8. `g2d_regs_v2.h` `G2D_BASE` is added by the driver (base = ioremap result);
   the register offsets above already include the block offsets.
