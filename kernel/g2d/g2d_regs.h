// SPDX-License-Identifier: GPL-2.0
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

#define G2D_FMT_NV12                  0x28
/* 0x29: UV-plane interleave order (NV12 vs NV21) is unverified on this HW. */
#define G2D_FMT_YUV420_UVC_U1V1U0V0   0x29
#define G2D_FMT_XRGB8888              0x04
#endif
