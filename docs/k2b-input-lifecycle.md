# K2B 输入回调与线程生命周期审计

这是离线源码审计和后续接入约束，不是已经可用的 Moonlight 后端。
固定 common-c 版本为 `b126e481a195fdc7152d211def17190e3434bcce`。
下列路径相对仓库根；CedarC 路径相对外部固定源码包，版本及来源见
[私有运行库记录](k2b-cedarc-runtime.md)。未连接开发板。

## 压缩输入的所有权

`third_party/moonlight-common-c/src/VideoStream.c` 的
`VideoDecoderThreadProc()` 在 `submitDecodeUnit()` 返回后立即调用
`LiCompleteVideoFrame()`。`VideoDepacketizer.c` 的后者释放整个碎片链；
启用 DIRECT_SUBMIT 时，`reassembleFrame()` 还会使用栈上的队列元素，
同样在回调后完成并释放输入。

所以异步后端必须在回调返回前复制完整压缩帧及所需元数据，不能保留
`DECODE_UNIT`、`LENTRY` 或源数据指针。access_unit 模块负责完整验证和
复制；input_queue 负责预分配存储及压缩帧借用。两者都不复制解码像素。

成功入队不等于完成解码。尤其 common-c 在 IDR 返回 `DR_OK` 时就记录
`idrFrameProcessed`；那只是其协议状态，不能作为 VPU 或显示统计。

## 两种关键帧请求并不等价

| 路径 | common-c 的行为 | 后端仍须负责 |
| --- | --- | --- |
| 回调返回 `DR_NEED_IDR` | `LiCompleteVideoFrame()` 调用 `requestDecoderRefresh()`，设置等待 IDR、清空 common-c 队列、要求接收线程在安全边界丢弃状态，再请求 IDR | 清理自己的待处理帧、阻止失效参考链继续送入解码器 |
| 工作线程调用 `LiRequestIdrFrame()` | `ControlStream.c` 清空参考帧失效请求并设置 IDR 请求事件 | 自己进入等待 IDR 状态；该 API 不清空后端队列，也不设置 depacketizer 的等待 IDR 状态 |

`Limelight.h` 明确指出异步请求不保证下一帧就是 IDR。
因此队列 FULL/非法输入时不能简单丢一帧后继续接受依赖它的 P 帧；
异步解码失败时也不能仅发 IDR 请求就继续解码旧队列。

后续接入需要统一的参考链恢复状态：在自身状态锁下标记等待 IDR，
丢弃待处理压缩帧，并处理已经借出/正在解码的旧帧。队列的
`discard_pending()` 保留借用帧，只解决存储安全，不会撤销已经发生的
解码操作。恢复代次、正在处理的帧和输出帧持有关系仍需上层协调。
只有拿到可接受的 IDR 并完成所需解码器恢复，才能重新放行参考链。
不要把队列计数或帧号直接当成跨重连唯一身份。

当前没有实现该恢复状态机，也不声明参考帧失效能力。已有 FIFO 本身
不会调用网络、CedarC、显示设备或自动请求 IDR。

## 停止与销毁必须分开

`Limelight.h` 允许 stop 回调之后仍有帧到达，cleanup 才保证不再提交。
`VideoStream.c:stopVideoStream()` 的实际顺序是：

1. 调用视频后端 stop。
2. 停止 depacketizer、打断并 join common-c 接收/解码等线程。
3. 关闭视频连接资源，调用视频后端 cleanup。

后端 stop 应标记停止、停止输入队列并唤醒自己的工作线程；不得在此
提前释放仍可能被最后一次 submit 访问的队列或回调上下文。停止后的
提交可安全丢弃，不能计入解码/显示成功；无需为停机丢弃反复请求 IDR。

cleanup 阶段必须先确保后端自己的工作线程退出，归还压缩输入借用，
再销毁队列。`k2b_input_queue_destroy()` 要求其他调用已静止；它不是
可与任意调用竞争的取消操作。CedarC 图片、DMA-BUF 和显示退役另有
生命周期，不能因为压缩输入已经 release 就一并释放。

### 初始化失败不是完整停止流程

`startVideoStream()` 有三种不同路径：

- setup 返回错误：直接返回，不替后端调用 cleanup。setup 必须自行
  回滚已经分配的资源。
- setup 成功但绑定 UDP 失败：直接 cleanup，没有 start/stop。
- start 后线程创建失败：stop，按已启动线程情况清理，再 cleanup。

因此 cleanup 必须能处理 setup-only 和部分启动，不假定 stop 必然
调用过。start 返回类型为 void，工作线程创建失败如何报告也必须在
接入时明确，不能静默留下“启动成功但永远不消费”的队列。

## 回调能力与延迟

DIRECT_SUBMIT 在接收线程执行回调，`Limelight.h` 要求非阻塞后端。
固定内存 FIFO 不等待空位、不会在持锁时调用设备，但 push 仍需锁和
有界压缩数据复制，不能称为 wait-free，也不自动证明适合声明该能力。
需要测量最坏回调耗时及锁竞争后再选择。初期不声明该能力时，
common-c 自己还有一个容量 15 的队列，端到端排队统计必须考虑两层。
不能同时提供 submit 回调又声明 PULL_RENDERER。

## CedarC reset 的边界

外部 `vdecoder/vdecoder.c:ResetVideoDecoder()` 调用 VideoEngineReset、
FbmFlush 和 SbmReset，并清除部分输入元数据；这不只是清空本地 FIFO。
`vdecoder/fbm/fbm.c:FbmFlush()` 从 valid-picture 队列取出图片，根据
decoder 持有标志移到 empty 队列或标记 already-displayed。它本身不是
“释放所有显示持有图片”的证据，也不证明 engine reset 与显示并发安全。

生产路径不能从网络回调并发 reset 工作线程中的解码器。需要结合真实
FBM 持有状态和显示退役规则验证恢复，不能因收到新 IDR 就假定旧输出
缓冲区已不被 DE33 扫描。显示部分仍受
[同步审计](k2b-disp-sync-audit.md) 的未解决条件约束。

## 待接入验证

- 队列满、非法帧和异步解码失败均进入一致的参考链恢复流程。
- 恢复期间的 P 帧、旧借用帧及迟到输出不会跨代次发布。
- stop 与最后一次 submit 竞争、setup-only cleanup、重复连接正常清理。
- 每个统计量区分收到、入队、提交解码、产出图片和实际显示；队列测试
  不替代板端 1920×1080、60 distinct fps 的验收。
