# K2B 厂商 disp 同步接口源码审计

日期：2026-09-24。仅离线源码审计，未连接或操作开发板。

源码根目录：`F:/work/source/aw-image-build/source/kernel/linux-5.4-h618`。
下文 `disp/` 指 `drivers/video/fbdev/sunxi/disp2/disp/`。
结论只适用于这份源码；板端运行内核的配置、实现一致性仍待核对。

## 结论与证据边界

厂商源码存在 `DISP_HWC_COMMIT` release fence 接口，不能再简单断言
“disp 完全没有 fence”。但当前实现没有把 fence 序号与某个图层配置、
DMA-BUF 或 RCQ 事务绑定；**尚不能据此安全归还 VPU 帧**。

| 证据 | 当前状态 | 能证明什么 |
| --- | --- | --- |
| `dev_composer.c` 和 H618 defconfig | 已核对 | 源码提供可选 composer fence |
| 板端运行配置、接口响应 | 待联网 | 当前镜像是否实际启用该接口 |
| fence 与硬件切换、旧帧退役关联 | 未验证 | 持续显示时能否安全复用缓冲区 |
| 真实解码与 HDMI 1080p60 | 未验收 | 仍须动态内容、逐帧证据与稳定性测试 |

静态彩条成功、60 Hz VSYNC、配置 ioctl 返回 0、软件读回图层 ID，
均不替代最后两项。此前的异常不能凭本次源码推演归因。

## 1. 编译入口及 UAPI

`drivers/video/fbdev/sunxi/Kconfig` 中 `DISP2_SUNXI_COMPOSER` 依赖
`DISP2_SUNXI`、选择 `SYNC_FILE`，默认 n；`disp/Makefile` 条件编译
`dev_composer.o`，`dev_disp.c` 条件调用 `composer_init()`。
`arch/arm64/configs/linux_h618_defconfig` 设置该项为 y。
**defconfig 不是板端当前内核配置**，目前没有后者的已核对副本。

`include/video/sunxi_display2.h` 定义 `DISP_HWC_COMMIT=0x0e`。
`DISP_HWC_CUSTOM=0x13` 在本次 disp 树检索中只有声明，未找到处理入口。
`dev_disp.c:disp_ioctl` 复制四个 native `unsigned long` 参数，然后
通过注册的扩展回调进入 `dev_composer.c:hwc_ioctl`。

| 参数 | 内容 |
| --- | --- |
| args[0] | screen，探针只限定 0 |
| args[1] | 下列子命令 |
| args[2] | 子命令相关值/用户指针 |
| args[3] | 未使用，置 0 |

子命令：

- 1 `NEW_CLIENT`：args[2] 指向接收 DE 时钟的 int。
- 2 `DESTROY_CLIENT`：停用客户端并释放所有等待 fence。
- 3 `ACQUIRE_FENCE`：args[2] 指向 `{ int fd; unsigned int count; }`。
- 4 `SUBMIT_FENCE`：args[2] **直接放序号数值，不是指针**。

以 AArch64 native ABI 为目标，不从 Windows unsigned long 大小或 compat
回调外推出 32 位用户态兼容性。扩展 ioctl 未注册时返回路径可能为 -1，
不能仅凭 `errno == ENOTTY` 判定“不支持”。

该客户端状态是每个 screen 的全局状态，不是每个打开 fd 的私有状态。
若已有 active client，NEW_CLIENT 返回 0 且不再写时钟输出；它不能授予
排他所有权。不能抢占别的显示客户端后随意 DESTROY。

## 2. fence 的实际含义

`hwc_aquire_fence()` 增加 timeline 序号，创建 dma_fence/sync_file，
返回 CLOEXEC fd 与 count。`hwc_submit()` 仅修改全局 submitted count。
`disp_composer_proc()` 在回调时将 current 更新为 submitted，并释放
严格早于 current 的 fence，而不是等于 current 的 fence。

在没有序号回绕、没有销毁/休眠的简单情形：

```text
submit(1) → 同步回调 → fence 1 仍等待
submit(2) → 同步回调 → fence 1 发信号，fence 2 仍等待
```

这是源码语义示例，不是板端时序实测。它符合“落后一帧的释放”意图，
但不证明后一帧的 DE 寄存器已生效。尤其：

- 没有参数将序号与 `LAYER_SET_CONFIG2`、图层或 DMA-BUF 原子绑定。
- DESTROY_CLIENT、suspend/no-output 路径会给全部 fence 发信号。
  因而 signal 也可能表示关闭/休眠，不能计为一次实际显示。
- 关闭 fence fd 不是停止 DMA；关闭 disp fd 也不是 composer 所有权协议。
- acquire 的 copy_to_user 失败只打印日志，仍可能返回成功；未来探针须
  初始化输出并验证 fd/count，不能故意传坏指针验证这一错误路径。
- 分配失败路径涉及已经创建的 sync_file，资源清理值得单独复核；
  不通过耗尽 fd/内存或故障注入去刺激厂商内核。

`hwc_dump()` 可打印 all/sub/cur/free/skip，但只对 active client 输出。
`dev_disp.c` 将其追加到 `attr` 属性组的 `sys` 输出；联网后先发现实际
sysfs 路径，不能把没有这一行直接解释为内核未编译 composer。

## 3. DE330 的 RCQ 路径是关键

`disp/de/disp_features.h` 为 DE V33X / SUN50IW9 选择 DE330 头文件；
`lowlevel_v33x/de330/de_feat.h` 定义 `RTMX_USE_RCQ (1)`。
这是当前源码 DE330 路径，不是板端配置/二进制已匹配的证明。

`disp/de/disp_display.c:sync_event_proc()` 在 RCQ 分支每次调用
`disp_int_process()`；`dev_disp.c` 将其接到已注册的 sync-finish 回调，
包括 composer。**此调用不是从 RCQ_FINISH 中断派发的。**
因此普通 VSYNC/composer current 前进，不能单独证明对应 RCQ 已完成。
非 RCQ 分支的 cfg/shadow protect 条件也不能套用到 DE330 RCQ 分支。

`disp/de/disp_manager.c` 中：

- `disp_mgr_apply()` 调用 reg_protect(true)，更新配置，再调用
  reg_protect(false)，且不传播 reg_protect 返回值。
- `disp_mgr_protect_reg_for_rcq(true)` 可能等待**上一次** RCQ；等待超时
  会停止 update、增加 skip 计数，仍返回 0。
- `reg_protect(false)` 启动本次 RCQ。SET_CONFIG2 返回不是本次完成确认。
- `disp_mgr_rcq_finish_irq_handler()` 收到 RCQ_FINISH 后标记
  `unmap_dmabuf=true`，并唤醒等待者。这比 VSYNC 更接近寄存器切换证据，
  但目前没有找到把它与指定 buffer/提交序号一起导出的用户态接口。

`dev_fb.c` 中 FBIO_WAITFORVSYNC 的实际等待被注释；不能靠其返回 0 排序。
`DISP_LAYER_GET_FRAME_ID` 在本次 disp 树检索中没有找到实现。
VSYNC timestamp 接口只证明有显示时序事件，且其 args[0] 是输出指针，
不是多数 disp 命令采用的 screen 参数。

## 4. DMA-BUF 导入与退役

`disp_mgr_set_layer_config2()` 在开始阶段消费 `unmap_dmabuf`，按旧的软件
图层配置执行 `disp_unmap_dmabuf()`，然后保存新配置、导入新 DMA-BUF、
执行 apply。导入失败可能禁用图层并增加错误计数，尾部仍返回成功。
因此返回 0 也不是“导入与显示都成功”的证据。

导入列表按图层/地址匹配做清理，不向用户态暴露逐缓冲区 release fence。
每次启用图层提交都会发生导入；重复提交相同地址不能假定会去重。
导出器的 map/unmap/detach 观测有助于确认驱动引用，但不能自动证明
驱动是在硬件真正停止读取以后才 unmap。

**待验证的时序风险推演（不是已复现缺陷）：**

1. A 的 RCQ 尚未完成，B 的 SET_CONFIG2 已开始并保存软件配置 B。
2. A 完成，在 B 的 apply 等待期间设置 `unmap_dmabuf=true`。
3. B 启动自己的 RCQ 后返回；C 的 SET_CONFIG2 消费该标志，并按软件
   配置 B 清理 A 的映射，而 B 的 RCQ 可能尚未完成。

RCQ_FINISH 路径没有非 RCQ 同步路径中的 `!setting` 条件。这使上述
顺序值得通过真实时序/地址关联排查；是否存在额外硬件顺序保证尚未知。
不要据此宣称它就是此前死机原因，也不要直接改厂商内核。

## 5. 网络恢复后的最小验证阶梯

以下是待执行清单，不是本次已经执行的操作。

1. 先保存启动 ID、运行内核配置、内核日志、现有进程/显示客户端和
   CMA/DMA-BUF 状态，核对本地源码与运行镜像；不启动串流。
2. 查找 sysfs 及可用符号，核实 composer 和 RCQ 路径。注意即使只读
   查询，打开后关闭最后一个 `/dev/disp` fd 也可能触发 HDMI 下电。
3. 在确认没有其他 composer 客户端后，短时验证 NEW/ACQUIRE 的有效
   返回、fence 等待/关闭和正常清理；限制次数，不跑重试循环。
4. 使用有限的 CPU 生成 NV12 多缓冲区和可辨帧号，先不归还/复用 VPU
   帧。关联提交序号、RCQ 完成、实际扫描地址与 map/unmap 事件；
   检查跳帧、暂停及停止时是否出现“有 fence、无对应切换”。
5. 只有证明旧帧已不再被读取后才启用缓冲区复用。停止时先确认图层
   停止读取，再销毁 composer；不能用 destroy 发出的信号证明 DMA idle。
6. 若现有接口不能提供可验证的关联，再提出最小观测/内核修改方案。
   本次不部署内核修改、不用固定延时替代同步证明。

主机配置测试仅解决 NV12 布局到真实厂商字段的转换。实际硬件的帧退役、
颜色、逐帧显示、60 fps 与长期稳定性仍是后续独立验收项目。
