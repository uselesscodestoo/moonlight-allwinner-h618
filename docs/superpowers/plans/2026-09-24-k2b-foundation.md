# K2B 构建基线与 NV12 帧契约实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 K2B 独立后端建立可重复的板端构建基线，并实现不会把 NV12 对齐填充或错误偏移误交给显示驱动的纯数据契约。

**Architecture:** 不改现有平台行为。先把布局验证做成无硬件依赖的 C 模块，在主机与 AArch64 板端运行同一测试；后续 CedarC 与 disp 都通过这一契约交接帧。

**Tech Stack:** C99、GCC、CMake、Git 子模块、板端 Ubuntu 22.04 开发包。

**Spec:** `docs/superpowers/specs/2026-09-24-k2b-cedarc-disp-design.md`（用户已确认）。

## 分期与完整目标

当前计划只覆盖可独立测试的基础组件，并非缩小最终目标。后续必须完成：

| 设计要求 | 后续交付/验证 |
| --- | --- |
| 固定 CedarC ABI、私有库、CMA 资源顺序 | 将已验证探针代码移植至独立运行库包，重跑 ABI 与逐帧像素回归 |
| 显示提交、生效、退役证据 | 对厂商同步/寄存器路径做源码审计，必要时在自有导出器增加有界观测；先短测 |
| Moonlight 有界队列及平台回调 | 独立 k2b 平台接入，仅声明已验证能力，网络回调不阻塞显示 |
| 生产路径动态显示与 60 fps | 逐帧身份和呈现证据，真实串流两轮 60 秒、一轮 10 分钟 |
| 退出、重连、音频、输入 | 故障注入单测与板端资源/功能检查 |
| 可独立维护 | 构建/启动/验收脚本、来源与版本清单、已知限制、日志 |

每个后续组件在开始编码前写出基于已查明接口的具体计划；同步证据未知时不编造退休条件。
当前计划完成不得标记线程目标完成。

## 文件结构

- `src/video/k2b/frame.h`：格式与布局契约，不依赖厂商内核头文件。
- `src/video/k2b/frame.c`：严格验证单 DMA-BUF、线性 NV12、1920×1080 可见画面。
- `tests/k2b/test_frame.c`：实际函数的正反例，无 mock 设备。
- `tests/k2b/Makefile`：独立宿主/板端测试入口，输出放仓库已有忽略目录 `build/`。
- `docs/k2b-bringup.md`：构建步骤、环境版本与当前尚未验证项。

## Task 1：建立原有代码的板端构建基线

- [x] 在 Git 自带 Bash 下递归初始化子模块并核对 gitlink。
- [ ] 模拟依赖安装，确认不移除包、不更换内核。实际安装只选构建所需包，不执行 dist-upgrade。
- [ ] 用包含子模块的源码归档传入此前不存在的 `/home/kickpi/projects/moonlight-embedded`；保留两个探针项目。归档不包含 `.git`、日志或运行库秘密配置。
- [ ] 在板端记录版本并配置原有 SDL 构建，不启动视频：

```sh
cmake -S . -B build/baseline -DENABLE_SDL=ON -DENABLE_X11=OFF -DENABLE_CEC=OFF -DENABLE_PULSE=OFF
cmake --build build/baseline -j2
```

预期：构建产出 `build/baseline/moonlight`。SDL 仅用于原有构建基线，绝不是 K2B 的达标显示路径。
若原有构建失败，先定位并记录失败；不将失败隐去或宣称回归通过。

## Task 2：以测试驱动实现严格 NV12 帧契约

接口固定如下（字段均是描述，不读取像素）：

```c
enum k2b_pixel_format { K2B_PIXEL_NV12 = 1 };
enum k2b_matrix { K2B_MATRIX_BT601 = 1, K2B_MATRIX_BT709 = 2 };
enum k2b_range { K2B_RANGE_LIMITED = 1, K2B_RANGE_FULL = 2 };
struct k2b_frame {
    int fd;
    enum k2b_pixel_format format;
    size_t allocation_bytes, y_offset, uv_offset;
    uint32_t stride, storage_height, crop_x, crop_y, width, height;
    enum k2b_matrix matrix;
    enum k2b_range range;
};
/* 0 = valid, -1 = invalid; no resource ownership transfer. */
int k2b_frame_validate(const struct k2b_frame *frame);
```

- [ ] 先写测试与可编译的最小拒绝桩；运行有效 1920×1088 分配/1920×1080 裁剪案例，观察断言失败，而不是编译报错。

测试有效输入：

```c
struct k2b_frame valid = {
    .fd = 0, .format = K2B_PIXEL_NV12,
    .allocation_bytes = 1920u * 1088u * 3u / 2u,
    .y_offset = 0, .uv_offset = 1920u * 1088u,
    .stride = 1920, .storage_height = 1088,
    .width = 1920, .height = 1080,
    .matrix = K2B_MATRIX_BT709, .range = K2B_RANGE_LIMITED
};
assert(k2b_frame_validate(&valid) == 0);
```

- [ ] 补充每项仅修改一个约束的反例：NULL、负 fd、非 NV12、非零 Y 起点、
  UV 起点与 stride×存储高不一致、短分配、奇数 stride/存储高/裁剪、
  裁剪越界、非 1920×1080、非法颜色枚举、超大字段。
- [ ] 增加有效例：1920×1080 紧凑分配、2048 步长、偶数非零裁剪且分配足够、
  BT601/BT709 与 limited/full 四种组合。
- [ ] 实现以下完整验证体，然后重跑测试：

```c
int k2b_frame_validate(const struct k2b_frame *f) {
    size_t y, uv;
    if (!f || f->fd < 0 || f->format != K2B_PIXEL_NV12 ||
        f->y_offset != 0 || f->width != 1920 || f->height != 1080 ||
        !f->stride || f->stride > 8192 || !f->storage_height ||
        f->storage_height > 8192 ||
        ((f->stride | f->storage_height | f->crop_x | f->crop_y) & 1u))
        return -1;
    if (f->width > f->stride || f->height > f->storage_height ||
        f->crop_x > f->stride - f->width ||
        f->crop_y > f->storage_height - f->height)
        return -1;
    if ((f->matrix != K2B_MATRIX_BT601 && f->matrix != K2B_MATRIX_BT709) ||
        (f->range != K2B_RANGE_LIMITED && f->range != K2B_RANGE_FULL))
        return -1;
    y = (size_t)f->stride * f->storage_height;
    uv = y / 2;
    return f->uv_offset == y && f->allocation_bytes >= y + uv ? 0 : -1;
}
```

上限保证中间尺寸计算在 32 位 size_t 也不会溢出。拒绝非零 Y 起点是因为
当前 disp 导入 ABI 不提供任意平面 offset；不是对所有 NV12 格式的泛化限制。

- [ ] Makefile 用 `-std=c99 -O2 -Wall -Wextra -Werror` 编译真实测试和 `frame.c`。
  `make -f tests/k2b/Makefile test` 在主机和板端均须以 0 退出。
- [ ] 用只运行测试的 ASan/UBSan 构建覆盖边界输入（支持该工具的 Linux 环境）。
- [ ] 审查后提交四个文件，不接入设备、不导出虚假的 k2b 平台能力。

## Task 3：记录基线与下一阶段接入点

- [ ] `docs/k2b-bringup.md` 记录固定子模块、构建命令、测试结果与可复现环境。
- [ ] 明确 `frame_validate` 不证明像素正确、不证明 fd 有效、不证明显示完成，
  它只是生产路径将复用的布局前置检查。
- [ ] 保持原分支 `4e870a2` 不动，核对本分支变更范围与原有脚本语法。
- [ ] 对实现做独立规格审查和质量审查，修复实际问题后才推进后续组件。
- [ ] 在本计划中更新完成项，保留完整串流目标未完成状态。

## 计划自检

当前阶段覆盖构建基线与布局约束；上表逐项保留了其余设计要求，没有将
它们当作当前阶段的已完成项。所有涉及硬件同步的条件仍须独立证明。
不新增泛化渲染框架，不复制像素，不改变旧后端默认行为。
