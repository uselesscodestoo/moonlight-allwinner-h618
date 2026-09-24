# K2B 有界压缩帧队列实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Moonlight 输入与受控解码线程之间有固定内存上限、明确借用生命周期及可停止的 FIFO。

**Architecture:** pthread mutex/condition 保护预分配环形队列；网络生产者不等待空位，单消费者借用队头直到 release。锁内只操作元数据或复制有界压缩码流，绝不执行解码、显示或设备等待。队列不替代上层 IDR 恢复状态机。

**Tech Stack:** C99、pthread、已测试的 access_unit 模块、Linux 原生测试与 AArch64 交叉编译。

本计划落实已批准设计的输入队列，不改变完整 1080p60 目标。
开发板离线，不执行 SSH、自动重连、设备探针或系统安装。

## 文件及接口

创建 `src/video/k2b/input_queue.h`、`input_queue.c`、
`tests/k2b/test_input_queue.c`；修改 `tests/k2b/Makefile` 增加
`test-queue`，不改变其他目标。主执行者另写 `docs/k2b-input-lifecycle.md`
并更新开发记录；实现者不改文档或平台代码。

```c
#define K2B_INPUT_QUEUE_MAX_SLOTS 8u
struct k2b_input_queue;
enum k2b_queue_result {
    K2B_QUEUE_OK = 0, K2B_QUEUE_EMPTY = 1, K2B_QUEUE_FULL = 2,
    K2B_QUEUE_STOPPED = 3, K2B_QUEUE_BUSY = 4, K2B_QUEUE_TOO_LARGE = 5,
    K2B_QUEUE_INVALID = -1, K2B_QUEUE_ERROR = -2
};
struct k2b_input_view {
    const unsigned char *data;
    struct k2b_access_unit unit;
    const struct k2b_input_queue *owner;
    uint64_t token;
};
struct k2b_input_stats {
    uint64_t accepted, released, rejected_full, rejected_input;
    uint64_t rejected_stopped, discarded;
    size_t slots, slot_bytes, reserved_bytes;
    size_t occupied, queued, high_watermark;
    unsigned int waiting;
    int leased, stopped;
};
int k2b_input_queue_create(struct k2b_input_queue **out,
                           size_t slots, size_t slot_bytes);
int k2b_input_queue_push(struct k2b_input_queue *q, const DECODE_UNIT *unit);
int k2b_input_queue_take(struct k2b_input_queue *q,
                         struct k2b_input_view *out, int wait);
int k2b_input_queue_release(struct k2b_input_queue *q,
                            const struct k2b_input_view *view);
int k2b_input_queue_discard_pending(struct k2b_input_queue *q);
int k2b_input_queue_stop(struct k2b_input_queue *q);
int k2b_input_queue_stats(struct k2b_input_queue *q,
                          struct k2b_input_stats *out);
int k2b_input_queue_destroy(struct k2b_input_queue **queue);
```

公开头文件包含 access_unit.h。单生产者、单消费者、可并发的控制/统计
调用；不支持 pthread_cancel 或从信号处理器直接调用。停止由普通线程
执行。所有传入指针必须有效，输出对象不得与队列内部/源对象重叠。

`create` 要求 out 非 NULL 且 *out 为 NULL，slots 为 1..8，slot_bytes 为
1..K2B_AU_MAX_BYTES。成功一次性分配队列及连续存储 slots*slot_bytes，
最大 32 MiB 压缩数据；失败保持 *out。分配/同步原语初始化失败返回 ERROR
并清理已完成的初始化；参数错误 INVALID。push/take/release 不 malloc。

## Task 1：测试驱动实现生命周期

- [x] 先创建可编译拒绝桩和最小测试，看到 create/单帧入队取出失败。
  先跑 RED，不等完整测试集写完才落文件；随后分批扩展。

最小成功链测试（实际测试使用已有风格的 CHECK 显式计数）：

```c
struct k2b_input_queue *q = NULL;
char bytes[] = {0, 0, 1, 0x41};
LENTRY e = {NULL, bytes, sizeof(bytes), BUFFER_TYPE_PICDATA};
DECODE_UNIT du = {0};
du.bufferList = &e; du.fullLength = sizeof(bytes);
du.frameType = FRAME_TYPE_PFRAME; du.colorspace = COLORSPACE_REC_709;
struct k2b_input_view v;
CHECK(k2b_input_queue_create(&q, 2, 64) == K2B_QUEUE_OK);
CHECK(k2b_input_queue_push(q, &du) == K2B_QUEUE_OK);
CHECK(k2b_input_queue_take(q, &v, 0) == K2B_QUEUE_OK);
CHECK(v.unit.bytes == sizeof(bytes) && !memcmp(v.data, bytes, sizeof(bytes)));
CHECK(k2b_input_queue_release(q, &v) == K2B_QUEUE_OK);
CHECK(k2b_input_queue_stop(q) == K2B_QUEUE_OK);
CHECK(k2b_input_queue_destroy(&q) == K2B_QUEUE_OK && q == NULL);
```

- [x] 实现不变量：私有结构包含 mutex、condition、固定 8 个槽位元数据、
  存储基址、slots/slot_bytes、head/tail/count、leased、stopped、
  next_token/active_token、统计计数与 waiting。count 包含正在借用的队头；
  `0 <= count <= slots`，queued=count-leased。存储只在 create/destroy 分配释放。

- [x] push 在锁内按顺序处理 stopped、full、完整 access_unit_copy；
  STOPPED 增 rejected_stopped，FULL 增 rejected_full，二者不读源数据。
  access_unit INVALID/NO_SPACE 分别映射 INVALID/TOO_LARGE，并增
  rejected_input；失败不发布槽位、不移动 head/tail/count。
  成功保存元数据，tail 环绕、count/accepted 增加、更新 high_watermark，
  signal condition。计数器不代表已解码/已显示。

核心发布步骤（已持锁，slot data 指向预分配的 tail 槽位）：

```c
int rc = k2b_access_unit_copy(&q->unit[q->tail],
    q->storage + q->tail * q->slot_bytes, q->slot_bytes, unit);
if (rc != K2B_AU_OK) {
    q->rejected_input++;
    result = rc == K2B_AU_NO_SPACE ? K2B_QUEUE_TOO_LARGE : K2B_QUEUE_INVALID;
} else {
    q->tail = (q->tail + 1) % q->slots;
    q->count++;
    q->accepted++;
    if (q->count > q->high_watermark) q->high_watermark = q->count;
    pthread_cond_signal(&q->available);
    result = K2B_QUEUE_OK;
}
```

- [x] take 检查 q/out 非 NULL、wait 为 0/1。锁内 STOPPED 优先于 BUSY；
  已有借用返回 BUSY。空队列 wait=0 返回 EMPTY；wait=1 在 while 条件
  中 wait 并正确处理伪唤醒，wait 前 waiting++、返回后 waiting--。
  有帧时增加不回绕的 token，借出 head 数据及元数据，但不减少 count。
  非 OK 保持 out 全部字节不变。token 用尽 UINT64_MAX 后返回 ERROR，
  不回绕/不借出（需上层停止重建）。pthread wait 错误同样返回 ERROR。

核心借用步骤：

```c
struct k2b_input_view result = {0};
q->active_token = ++q->next_token;
q->leased = 1;
result.data = q->storage + q->head * q->slot_bytes;
result.unit = q->unit[q->head];
result.owner = q;
result.token = q->active_token;
*out = result;
```

- [x] release 验证 leased、owner==q、token==active_token、data==head 数据。
  错误/重复/旧 token 或跨队列的 view 返回 INVALID 且不改变队列。
  正确 release 才 head 环绕、count--、leased=0、released++。
  view 数据仅在成功 take 到成功 release 之间有效；消费者不修改借用字节。
  这里归还的是压缩输入槽位，绝不是 VPU 画面或 DMA-BUF 退役。

- [x] discard_pending 保留借用帧，仅丢弃排队帧；stop 设置终止状态，
  执行同样丢弃并 broadcast。两者幂等；stop 后 push/take 均 STOPPED，
  但 release 仍允许完成。共用锁内清理逻辑：

```c
size_t keep = q->leased ? 1 : 0;
q->discarded += q->count - keep;
q->count = keep;
q->tail = (q->head + keep) % q->slots;
```

- [x] stats 锁内复制计数和当前状态；reserved_bytes=slots*slot_bytes，
  occupied=count、queued=count-leased、high_watermark 不因丢弃下降。
  返回计数允许 uint64_t 模加法，不将统计值当作唯一帧身份。
- [x] destroy 要求调用者已使其他调用静止并 join 工作线程；不是并发
  取消函数。未 stop、仍 leased 或 waiting 非零时返回 BUSY，保留对象。
  正确销毁 pthread 原语/存储后 *queue=NULL。NULL queue 或 *queue 为
  NULL 返回 INVALID。不把已销毁对象的陈旧指针当成受支持输入。

## Task 2：边界与线程验证

- [x] 验证参数边界/NULL、create 输出已占用、EMPTY 输出不变、FIFO、
  环绕复用、源修改后数据独立、槽位总数包括 lease、FULL 时 lease 字节
  不变、容量过小与非法后片不发布、重复/陈旧/跨队列 release 不改变状态。
- [x] 验证有 lease 的 discard_pending/stop 仍保持可读，stop 可重复、
  stop 后拒绝 push/take、归还最后 lease 后可销毁；未停止/有 lease 时
  destroy BUSY；精确检查 accepted/released/discarded/各拒绝计数与水位。
- [x] 实际 pthread 消费者进入等待（通过 waiting 统计观测），分别由
  push 和 stop 唤醒。用单生产者/消费者至少 10000 帧压力验证 FIFO 和
  leased 期间数据不变，生产者遇 FULL 只在测试中重试；生产回调不这样重试。
  测试线程不使用共享非原子计数；汇总在 pthread_join 后进行。
  测试用进程级 alarm 或外部 timeout 限定失败耗时，不访问设备。
- [x] test-queue 用 -pthread 链接真实 access_unit.c/input_queue.c，
  固定 Limelight.h 依赖，无厂商头文件。所有检查在 NDEBUG 保持启用。

```sh
make -B -f tests/k2b/Makefile test-queue CC=cc BUILD_DIR=build/k2b-linux
make -B -f tests/k2b/Makefile test-queue CC=cc BUILD_DIR=build/k2b-linux-ndebug CPPFLAGS=-DNDEBUG
```

- [x] 原生 ASan/UBSan 强制重建运行；尝试 TSan 并如实记录工具可用性，
  不把运行环境失败写成并发验证通过。AArch64 编译，不执行目标 ELF。
- [x] 独立规格审查后再质量审查，处理发现，主执行者复跑和提交检查点。

## Task 3：实际回调接入审计（主执行者）

- [x] 核对固定 common-c 的 submitDecodeUnit 源生命周期、DR_NEED_IDR、
  异步 LiRequestIdrFrame 与 stop/cleanup 顺序，写入生命周期文档。
- [x] 明确队列 FULL/非法输入后仍需上层参考链恢复门控，队列本身不
  自动请求 IDR，不假定普通 P 帧能独立解码；不新增虚假的可用 k2b 平台。
- [x] 更新构建/测试记录，保持整体目标未完成和板端离线状态。

## 自检

队列内存、借用帧和停止规则明确；与已有完整帧复制 API 名称一致。
线程容量是可配置的工程上限，不把 8 槽当作板端已验证的最佳值。
未生成设备同步假设；显示持有与退役仍按此前审计单独验证。

## 完成记录

实现提交 `c3485eb`；回调生命周期审计提交 `afefb7f`。
普通/NDEBUG、ASan+UBSan、TSan 均在本地 Linux 实际通过，12,000 帧
双线程压力测试通过，AArch64 只编译链接未执行。先规格审查、后质量
审查均无待修复项。token 耗尽和资源初始化失败分支只有源码检查，
未做故障注入。日志与命令见 `docs/k2b-bringup.md`。
本计划完成仅指输入队列阶段，生产后端和实际 1080p60 目标仍未完成。
