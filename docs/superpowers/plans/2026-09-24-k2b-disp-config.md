# K2B disp 配置转换与同步接口审计实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 离线实现已验证 NV12 描述到真实厂商 disp ABI 的配置转换，并审计可用于持续显示的同步接口。

**Architecture:** 纯转换函数消费 `frame.h`，不打开 fd、不调用 ioctl；实际厂商头文件由构建参数显式提供，不手写替代 ABI。硬件提交/退役仍需独立证据，不因配置测试通过而启用持续显示。

**Tech Stack:** C99、GCC、GNU make、厂商 `sunxi_display2.h`。

网络已中断，用户要求尽可能离线推进。禁止本阶段 SSH、自动重连和上板测试。
完整目标仍为真实 Moonlight 1920×1080、60 fps，并保留基础计划中的后续交付表。

## 文件结构

- `src/video/k2b/disp_uapi.h`：厂商头文件的最小 userspace 类型包装。
- `src/video/k2b/disp_config.h`、`disp_config.c`：纯配置转换。
- `tests/k2b/test_disp_config.c`：真实 ABI 配置字段与失败不修改输出测试。
- `tests/k2b/Makefile`：增加显式 `test-disp` 目标，不破坏原有无厂商依赖测试。
- `docs/k2b-disp-sync-audit.md`：源码同步审计结果、可证明内容及硬件待验证项。

## Task 1：先测试，再实现配置转换

- [ ] 新建测试与可编译拒绝桩，先观察有效帧转换返回失败的 RED。
- [ ] 使用真实头文件，不提交厂商完整头文件、不静默使用虚构 ABI。
  `disp_uapi.h` 提供以下包装，并加头文件保护：

```c
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef int32_t s32;
#include <video/sunxi_display2.h>
```

头文件取自本地厂商树的 `include`，SHA256：
`f573cf66d2aee34373dd4b46c8d16aaaad4d3fd4264caab22abb8f2664a570f4`。

接口（在有保护的 `disp_config.h` 中声明）：

```c
#include "frame.h"
#include "disp_uapi.h"
int k2b_disp_config_prepare(struct disp_layer_config2 *out,
                            const struct k2b_frame *frame,
                            uint32_t frame_id);
```

- [ ] 有效例验证 fd=0、存储 1920×1088、实际 1920×1080、UV 960×544、
  目标屏幕 1920×1080、全部四种 matrix/range、紧凑分配、2048 步长、
  非零偶数 crop 的 32.32 转换、frame_id=0/UINT32_MAX，以及禁用压缩/3D/ATW。
- [ ] 无效例复用布局检查：NULL 输出/输入、负 fd、错误 UV 起点、短分配、
  错尺寸、非法颜色；输出预填 0xa5 后验证失败不会修改任何字节。
  所有检查使用显式计数/非零退出，不依赖 assert。
- [ ] 实现完整转换体：

```c
int k2b_disp_config_prepare(struct disp_layer_config2 *out,
                            const struct k2b_frame *f, uint32_t frame_id) {
    if (!out || k2b_frame_validate(f) != 0) return -1;
    memset(out, 0, sizeof(*out));
    out->enable = true;
    out->channel = 0;
    out->layer_id = 0;
    out->info.mode = LAYER_MODE_BUFFER;
    out->info.zorder = 31;
    out->info.alpha_mode = 1;
    out->info.alpha_value = 255;
    out->info.screen_win.width = 1920;
    out->info.screen_win.height = 1080;
    out->info.id = frame_id;
    out->info.fb.fd = f->fd;
    out->info.fb.trd_right_fd = -1;
    out->info.fb.metadata_fd = -1;
    out->info.atw.cof_fd = -1;
    out->info.fb.size[0].width = f->stride;
    out->info.fb.size[0].height = f->storage_height;
    out->info.fb.size[1].width = f->stride / 2;
    out->info.fb.size[1].height = f->storage_height / 2;
    out->info.fb.format = DISP_FORMAT_YUV420_SP_UVUV;
    out->info.fb.color_space = f->matrix == K2B_MATRIX_BT709
        ? (f->range == K2B_RANGE_FULL ? DISP_BT709_F : DISP_BT709)
        : (f->range == K2B_RANGE_FULL ? DISP_BT601_F : DISP_BT601);
    out->info.fb.eotf = DISP_EOTF_GAMMA22;
    out->info.fb.flags = DISP_BF_NORMAL;
    out->info.fb.scan = DISP_SCAN_PROGRESSIVE;
    out->info.fb.crop.x = (int64_t)f->crop_x << 32;
    out->info.fb.crop.y = (int64_t)f->crop_y << 32;
    out->info.fb.crop.width = (int64_t)f->width << 32;
    out->info.fb.crop.height = (int64_t)f->height << 32;
    return 0;
}
```

`size[0].width` 在 NV12 中是每像素一字节，所以与字节步长相等；UV 平面
每个采样点两字节，因此宽为 stride/2。厂商 `disp_set_fb_info` 据此计算
UV 起点为 stride×存储高；不能把存储高写成可见高。
EOTF 沿用已验证静态探针的 SDR GAMMA22；不在此函数宣称 HDR 支持。
`info.id` 仅是提交标签，不把读回该值当成硬件显示证明。

- [ ] Makefile 新增 `test-disp`，仅该目标要求 `K2B_VENDOR_HEADERS`；缺失
  路径时先输出明确错误并失败。原来的 `test` 仍不需要厂商文件。
  用 `-idirafter "$(K2B_VENDOR_HEADERS)"` 查找真实头文件，生成
  `build/k2b-tests/test_disp_config`（Windows 加 .exe）。
- [ ] 在主机正常/NDEBUG 两次强制重建并运行两个测试目标。命令：

```sh
make -B -f tests/k2b/Makefile test test-disp \
  K2B_VENDOR_HEADERS=F:/work/source/aw-image-build/source/kernel/linux-5.4-h618/include
make -B -f tests/k2b/Makefile test test-disp CPPFLAGS=-DNDEBUG \
  K2B_VENDOR_HEADERS=F:/work/source/aw-image-build/source/kernel/linux-5.4-h618/include
```

- [ ] 独立规格审查、质量审查，处理问题并提交；硬件 ABI 与动态画面仍待上板验证。

## Task 2：同步接口源码审计

- [ ] 对照 `dev_composer.c`、`dev_disp.c` 和 `disp_manager.c`，检查
  DISP_HWC_COMMIT/DISP_HWC_CUSTOM 是否暴露真正的 release fence 或逐帧确认。
  记录配置编译条件、参数 ABI、单帧关联、等待语义、失败和关闭路径。
- [ ] 区分“源码存在”“板端内核启用”“运行时返回有效完成信号”三种证据。
  板端当前离线，后两种缺失时明确标记未验证，不编造可用性结论。
- [ ] 若可复用 composer fence，给出最小只读/短测验证方案；若不可用，
  明确导出器观测与硬件寄存器证据各自能证明什么，不给出未经证明的回收代码。
- [ ] 审计文档与配置测试结果写入开发记录，准备网络恢复后的核对清单。

## 自检

当前阶段只落实已知 ABI 的配置转换和同步证据调查。没有用测试替代实际
解码/显示验收；不启动硬件，不新增软件渲染回退，不改变旧平台默认选择。
