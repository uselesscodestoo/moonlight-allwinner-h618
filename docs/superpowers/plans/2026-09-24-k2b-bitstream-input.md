# K2B 完整码流帧与 CedarC ABI 接入计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现生产后端可复用的 Moonlight 完整帧复制边界，并用真实 AArch64 编译器验证固定 CedarC ABI。

**Architecture:** 网络端传入的 DECODE_UNIT 链在回调返回前复制到调用者拥有的有界槽位。先完整验证再写入，压缩数据按原字节保留。ABI 检查只编译、不执行驱动；本阶段不接入平台能力，不启动硬件。

**Tech Stack:** C99、固定 moonlight-common-c 头文件、GNU make、WSL Linux GCC / AArch64 GCC。

已批准设计中的私有运行库接入先完成其输入契约和 ABI 前置检查。探针内存
实现含 abort/残留缓冲清理，不能直接作为生产实现复制进来。完整目标仍
包含有界线程队列、内存所有权、已证明的显示退役与真实 1080p60 串流；
本计划不是对完整目标的缩减。开发板离线，不进行 SSH 或网络重连。

## Task 1：完整帧拼接

文件：`src/video/k2b/access_unit.h`、`access_unit.c`、
`tests/k2b/test_access_unit.c`，修改 `tests/k2b/Makefile` 增加 `test-au`。
头文件使用真实 `<Limelight.h>`，不复制第三方结构。

```c
#define K2B_AU_MAX_BYTES (4u * 1024u * 1024u)
enum k2b_au_result { K2B_AU_OK = 0, K2B_AU_INVALID = -1,
                     K2B_AU_NO_SPACE = -2 };
struct k2b_access_unit {
    size_t bytes;
    int64_t pts_us;
    uint64_t receive_us, enqueue_us;
    int frame_number, frame_type;
    uint8_t colorspace;
};
int k2b_access_unit_copy(struct k2b_access_unit *out, void *storage,
                         size_t capacity, const DECODE_UNIT *unit);
```

调用者保证 out、storage、源链节点/源数据彼此不重叠，且调用期间源链
稳定、源指针可读、storage 实际可写 capacity 字节。函数不分配、不拥有
指针、不缓存源指针；成功后的存储不依赖 Moonlight 源生命周期。
4 MiB 是当前输入上限，与探针 VBV 配置一致，不声称支持任意大码流帧。
失败后调用者决定 DR_NEED_IDR/队列策略；本层不调用网络或 CedarC API。

- [x] 写显式计数测试和可编译拒绝桩，先确认有效单片/P 帧、SPS+PPS+IDR
  多片失败；随后实现。测试不使用 assert，不 mock 设备。
- [x] 完整验证成功后才 memcpy；输出元数据/存储失败均不变。
  核心实现按以下顺序，头文件保护和必要 includes 按 C99 补齐：

```c
if (!out || !storage || !unit || unit->fullLength <= 0 ||
    (size_t)unit->fullLength > K2B_AU_MAX_BYTES || !unit->bufferList ||
    unit->presentationTimeUs > INT64_MAX || unit->hdrActive ||
    (unit->frameType != FRAME_TYPE_PFRAME && unit->frameType != FRAME_TYPE_IDR) ||
    (unit->colorspace != COLORSPACE_REC_601 && unit->colorspace != COLORSPACE_REC_709))
    return K2B_AU_INVALID;
size_t expected = (size_t)unit->fullLength, total = 0;
for (const LENTRY *e = unit->bufferList; e; e = e->next) {
    if (!e->data || e->length <= 0 || (size_t)e->length > expected - total ||
        (e->bufferType != BUFFER_TYPE_PICDATA && e->bufferType != BUFFER_TYPE_SPS &&
         e->bufferType != BUFFER_TYPE_PPS)) return K2B_AU_INVALID;
    total += (size_t)e->length;
}
if (total != expected) return K2B_AU_INVALID;
if (capacity < expected) return K2B_AU_NO_SPACE;
size_t offset = 0;
for (const LENTRY *e = unit->bufferList; e; e = e->next) {
    memcpy((unsigned char *)storage + offset, e->data, (size_t)e->length);
    offset += (size_t)e->length;
}
struct k2b_access_unit result = {0};
result.bytes = expected;
result.pts_us = (int64_t)unit->presentationTimeUs;
result.receive_us = unit->receiveTimeUs;
result.enqueue_us = unit->enqueueTimeUs;
result.frame_number = unit->frameNumber;
result.frame_type = unit->frameType;
result.colorspace = unit->colorspace;
*out = result;
return K2B_AU_OK;
```

每个合法链节点长度至少 1，累计始终不大于 expected；循环链也会在有限
遍历后因超出 fullLength 被拒绝。仅验证容器和声明的 H.264/SDR 类型，
不解析 NAL 内容，不重新生成起始码，不把它称为 H.264 语法验证器。

- [x] 测试逐字节一致、guard 区不变、复制后修改源不影响副本、元数据
  0/MAX 边界；4 MiB 成功/加 1 拒绝；NULL、0/负长、空链、空数据、
  0/负片长、和大于/小于 fullLength、INT_MAX 片长、链成环、HDR、
  2020/未知颜色、未知帧类型、VPS/未知片类型、PTS 超 INT64_MAX、
  容量 0/差 1，均检查失败不写。错误出现在后续片也不能先复制前片。
- [x] `test-au` 使用固定子模块 include，无厂商依赖，产物放 build/。
  原 test/test-disp 保持可运行。普通/NDEBUG 使用 -B 强制重建。
- [x] Linux 原生普通/NDEBUG/ASan+UBSan 运行；AArch64 编译但不执行。
  独立规格审查后再质量审查，提交五文件内的实际变更。

## Task 2：真实 CedarC ABI 编译检查

新增 `tests/k2b/check_cedar_abi.c`，由主执行者处理，与 Task 1 文件不重叠。
使用真实外部 vdecoder.h；不复制或再分发二进制。编译条件限定 Linux
AArch64、TINA_LINUX_SUPPORT=0，并固定已验证结构尺寸及偏移。

```c
#include <stddef.h>
#include "vdecoder.h"
#if !defined(__linux__) || !defined(__aarch64__)
#error "K2B CedarC ABI check requires Linux AArch64"
#endif
#if !defined(TINA_LINUX_SUPPORT) || TINA_LINUX_SUPPORT != 0
#error "K2B CedarC ABI requires TINA_LINUX_SUPPORT=0"
#endif
typedef char k2b_vconfig_size[(sizeof(VConfig) == 224) ? 1 : -1];
typedef char k2b_stream_size[(sizeof(VideoStreamInfo) == 72) ? 1 : -1];
typedef char k2b_veops_offset[(offsetof(VConfig, veOpsS) == 152) ? 1 : -1];
typedef char k2b_veself_offset[(offsetof(VConfig, pVeOpsSelf) == 160) ? 1 : -1];
int main(void) { return 0; }
```

- [x] 先在错误 TINA_LINUX_SUPPORT=1/宿主架构上确认失败，再用真实
  AArch64 gcc、TINA_LINUX_SUPPORT=0 编译成功，不运行目标 ELF。
- [x] 外部 include：libcedarc-tina/include、base/include 及
  base/include/gralloc_metadata；按真实头文件依赖补入必要目录。
  用 readelf/file 核对目标架构。同时交叉编译已有 frame/disp 配置测试。
- [x] 更新 docs/k2b-bringup.md 和此清单，明确编译成功不能证明
  库加载、内核 ioctl ABI、生产内存层或实际解码/显示正确。

## 自检

计划覆盖批准设计的码流拼接和固定 ABI 前置条件；未覆盖项仍明确保留。
与显示同步调查独立，不因主机单测通过而打开持续显示，也不导出可选
k2b 平台来掩盖后端尚未接好的事实。

## 执行结果

`81fd00a` 提交固定核心库清单、ELF 依赖记录及 AArch64 编译 ABI 检查。
四个核心库哈希与固定归档一致；错误 TINA 宏、错误宿主架构负例均拒绝，
正确配置编译成功。新增检查本身不加载任何 CedarC 运行库。

`7f6d0a6` 提交完整帧复制：拒绝桩 11/189 检查按预期失败，实现后 272
检查通过。Windows、Linux 普通/NDEBUG 均通过；主执行者在提交后重跑
Linux 59/272/396 项布局/码流/配置检查及三组 ASan/UBSan，均通过。
四个测试/ABI 程序已交叉编译为 ARM aarch64 ELF，但未执行目标程序。

独立规格审查、随后独立质量审查通过，无遗留发现。原始分支保持
`4e870a2` 且工作区干净。开发板未连接、未同步、未运行新代码。
完整后端目标保持未完成：下一步仍需有界队列、受控解码线程、生产
内存与运行库接入、显式平台回调及已证明的显示持有/退役协议。
