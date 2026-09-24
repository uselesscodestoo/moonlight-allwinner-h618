# CedarC 内存会话与厂商驱动失败边界

本页是离线源码审计和内存适配器的接入约束，不是新代码已经通过板端
解码/显示验证。外部 CedarC 与厂商内核来源见
[运行库记录](k2b-cedarc-runtime.md)。

## 为什么不用探针的 open/close 原样实现

探针 `cedar_mem.c` 在部分失败时 abort，最后一个 close 会尝试释放所有
残留分配。生产后端有显示持有，不能把“调用了 close”当成“所有设备都
已停止访问”。同时 ScMemOpsS 的 close、pfree 和 flush_cache 没有返回值，
失败不能单靠函数返回传播给工作线程。

适配器因此采用独立 begin/end 会话和首错诊断：先 begin 取得额外 VE
引用，再初始化 CedarC；ScMemOps open/close 只维护内存使用者引用。
销毁解码器以后，只有引用及所有分配均为零，end 才尝试释放自己的 VE
引用。残留不被强制回收。工作线程必须在 CedarC 操作之后读取内存状态，
不能只检查 CedarC 返回值，也不能因一次 status 为零就跳过后续检查。

正常连接期望顺序：

```text
memory_begin (ENGINE_REQ)
  -> InitializeVideoDecoder (ScMemOps.open, 分配与导入)
  -> 解码 / 保留图片 / 显示与确认退役 / ReturnPicture
  -> DestroyVideoDecoder (pfree, ScMemOps.close 等)
memory_end (零引用 + 零分配 -> ENGINE_REL -> close)
```

这个顺序不是完整异常退出证明；下述 vendor 代码仍有无法靠该适配器
消除的不确定性。pin 只保护分配不被 pfree，不会阻止 ReturnPicture 后
VPU 重写同一图片；显示持有期间仍必须保留 CedarC 的图片所有权。

## 内核事实：close 也有副作用

厂商 `drivers/media/cedar-ve/cedar_ve.c`：

- `enable_cedar_hw_clk()` 在开启时钟之后初始化全局 DMA 链表。
  `IOCTL_ENGINE_REQ` 的常规分支增加 ref_count，首个引用调用此函数。
- `cedardev_release()` 无条件进入 `unmap_dma_buf_addr(1, 0, 0)`；
  不能把打开 VE 但尚未成功 request 的 fd 随手 close 当成无害回滚。
- `unmap_dma_buf_addr()` 的循环最终比较 `buf_info->addr == addr`。
  all 分支虽计算 tmp_addr，但实际比较仍使用传入 addr，不能假定
  release 的 all 分支能可靠清理本进程所有非零地址的导入。
- `IOCTL_ENGINE_REL` 先减全局引用，再可能因计数/时钟问题返回错误。
  收到错误后重发 REL，可能再次减引用，不能盲目重试。

因此在 VE 已打开但 request/身份检查不确定，以及 REL 错误时保留故障
状态和描述符，要求上层停止推进并报告。用户结束进程或断电仍会触发
操作系统/硬件级清理；用户态库不能承诺使异常进程退出安全，也不能
自动修复驱动全局引用或已坏的链表。

额外限制：常规 ENGINE_REQ 分支没有把 `enable_cedar_hw_clk()` 的返回值
传出去。ioctl 返回成功本身不能证明实际时钟全部启用或 DMA 链表正常；
需要硬件阶段检查干净内核状态及日志。适配器的型号/版本检查不是对
所有驱动故障、外部进程或修改后同版本内核的鉴定。

## DMA 操作失败为什么需要保留状态

`IOCTL_MAP_DMA_BUF` 先调用 `map_dma_buf_addr()`（导入、记录 fd/地址/pid
并加入链表），再 copy_to_user 返回地址。后者出错不会自动撤销前面的
登记。因而 MAP 的失败返回可能对应“未导入”，也可能对应“内核已导入但
用户未得到可靠地址”；不能用未确认的地址进行 UNMAP，更不能立即 REL。

常规释放应使用原 fd 和已确认地址显式 UNMAP，再 munmap/close。UNMAP
返回成功是此驱动路径的执行结果，但驱动并不报告是否找到了匹配节点；
正常正确元组及单实例所有权是这里的前提，不是通用逐缓冲区证明。
实际是否无残留仍需后续板端可观测计数核对。

适配器对不确定 MAP/UNMAP/munmap 状态隔离资源，不自动重试；状态中的
隔离计数和 live_bytes 必须包含它们，不能通过不返回分配指针而漏记。
Linux close 错误不触发再次 close，以免关闭已经复用的 fd。

## CedarC 初始化失败并非事务式回滚

固定外部 `vdecoder/vdecoder.c` 中：

- `InitializeVideoDecoder()` 先调用 CdcVeInit，随后才 CdcMemOpen。
  VConfig 中 memops 可以显式指定，但源码会重新设置 veOpsS。
- `err_exit` 会清空 context 中部分 VE 指针再返回 UNSUPPORTED；这并不
  构成已完成所有 VE/SBM/内存资源清理的证明。
- `DestroyVideoDecoder()` 先 VideoEngineDestroy，再 CdcMemClose，再在
  对应标志/指针存在时 CdcVeRelease，最后释放 context。

所以内存 end 只能管理适配器自己持有的资源和 VE 引用，不能冒充
libVE 全部内部引用已归还的证明。生产解码包装层必须区分初始化成功与
中途失败；首次接入时验证失败日志和引用回收，不自动循环重启一个
可能残留内核状态的解码器。这里没有擅自修改外部源码或厂商内核。

## 测试证据边界

离线测试在系统调用边界包装设备返回，仍运行真实会话、分配登记、
范围检查和 pin 逻辑。它可以检查调用顺序、故障后有没有误释放、正常
结束是否无残留，但不模拟真实 DMA、时钟、VPU 或 DE 行为。
异常测试保留资源是明确的故障策略，不应记为正常结束零残留。
实际解码、图片哈希、显示退役和 1080p60 串流仍必须上板验证。
