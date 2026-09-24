# K2B CedarC 会话内存适配实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为真实 CedarC ScMemOpsS 提供项目内 CMA/DMA-BUF 内存实现，代替会 abort/释放残留的探针代码。

**Architecture:** 单实例、mutex 保护的显式会话；begin 在 CedarC 初始化前取得 VE 引用，end 仅在 CedarC 引用和全部分配消失后释放 VE。真实 syscall 路径使用厂商头文件；测试只在系统调用边界做链接包装，不替换分配登记、状态和所有权逻辑。异常不确定时隔离资源并保留首个错误，不强制释放。

**Tech Stack:** C99、pthread、真实外部 sc_interface.h / cedar_ve.h、Linux linker --wrap、AArch64 交叉编译。

本计划落实已批准设计的内存层；开发板离线，不连接设备、不改内核、不安装库。
仅此会话适配不能证明动态显示退役或完整 Moonlight 后端已实现。

## 文件边界

- 创建 `src/video/k2b/cedar_memory.h`：会话、诊断、借用描述符及 pin API。
- 创建 `src/video/k2b/cedar_memory.c`：真实 ScMemOpsS 和 Linux syscall 路径。
- 创建 `src/video/k2b/cedar_uapi.h`：包含外部真实 cedar_ve.h，检查已用结构/命令 ABI；声明项目自有 heap ioctl，不复制厂商整份头文件。
- 创建 `tests/k2b/test_cedar_memory.c`：包装系统调用并故障注入，运行真实适配器。
- 修改 `tests/k2b/Makefile`：独立 test-memory 目标，不改变其他目标依赖。
- 主执行者更新 `docs/k2b-cedarc-runtime.md`、`docs/k2b-bringup.md`，实现者不改文档。

## Task 1：显式会话与真实 CedarC 接口

- [ ] 写最小拒绝桩与以下成功链测试，确认 RED 后实现，不先写完整实现。

```c
CHECK(k2b_cedar_memory_begin() == 0);
struct ScMemOpsS *ops = MemAdapterGetOpsS();
CHECK(ops->open() == 0);
void *p = ops->palloc(5000, NULL, NULL);
CHECK(p != NULL);
ops->pfree(p, NULL, NULL);
ops->close();
CHECK(k2b_cedar_memory_end() == 0);
```

公开接口：

```c
struct k2b_cedar_buffer {
    const void *base;
    size_t bytes, offset;
    int fd;
    uint32_t ve_address;
};
struct k2b_cedar_pin {
    struct k2b_cedar_buffer buffer;
    uint64_t token;
};
struct k2b_cedar_memory_status {
    size_t allocations, live_bytes, peak_bytes, pinned, quarantined;
    unsigned int references;
    int active, error;
};
int k2b_cedar_memory_begin(void);
int k2b_cedar_memory_end(void);
int k2b_cedar_memory_status(struct k2b_cedar_memory_status *out);
int k2b_cedar_memory_describe(const void *ptr, struct k2b_cedar_buffer *out);
int k2b_cedar_memory_pin(const void *ptr, struct k2b_cedar_pin *out);
int k2b_cedar_memory_unpin(const struct k2b_cedar_pin *pin);
struct ScMemOpsS *MemAdapterGetOpsS(void);
struct ScMemOpsS *SecureMemAdapterGetOpsS(void);
int MemAdapterGetDramFreq(void);
```

所有 int API 为成功 0、失败 -1 并设 errno；describe/pin 失败不修改输出。
`status.error` 为会话首个正 errno，0 表示无错误。不会因后续成功/新 begin
清除此错误；错误会话不得自动重开。本进程唯一实例，普通线程调用；
不支持 signal-handler、fork 后继承活会话、pthread_cancel。正常 end 后可
再次 begin；正常重连也不重用 pin token。

- [ ] begin 要求会话未活动、首错为 0。只接受 uname release=5.4.125、
  machine=aarch64、device-tree model 恰为 KICKPI K2B、页大小 4096；
  拒绝在打开 VE 前完成。先开 heap，再开 VE，均 O_RDWR|O_CLOEXEC，
  fstat 确认字符设备，然后 ENGINE_REQ。成功 active=1、references=0。
  model 文件读/关闭错误应传播，不接受截断或附加非 NUL 数据。
  heap open/类型验证失败、VE open 失败只清理已知安全的 heap fd，记录首错；
  VE fd 已打开而 fstat/类型验证失败时，保留该 fd 和故障会话，不假设
  未 ENGINE_REQ 的 Cedar release 安全（release 也会访问全局 DMA 链表）。
  ENGINE_REQ 一旦返回错误，其副作用不确定，保留设备 fd 和故障会话，
  不自动 REL/关闭 VE/重试。begin 活动时 EBUSY，不破坏原会话。
- [ ] ScMemOps open/open2 仅在健康活动会话内增加 references，溢出报错；
  close 只减少引用，不释放分配，不关闭设备。下溢记录 EINVAL，不能 abort。
  分配要求活动、references>0、无首错。end 在仍有引用/分配时 EBUSY，
  不调用 REL；有不确定 ENGINE 状态同样拒绝结束。正常空会话 end 按
  ENGINE_REL、close VE、close heap 顺序执行。REL 失败不重试、不关闭
  VE，保留故障会话；Linux close 错误记录但不重试 fd。没有强制清理 API。
- [ ] 保留真实 ScMemOpsS 全部字段类型，导出 MemAdapterGetOpsS；secure
  返回 NULL，DramFreq 返回 -1。提供 open2/debug/get_fd_by_vir，未支持的
  no-cache 或 fd 反向导入回调返回明确 ENOTSUP，不留下可调用 NULL。
  setup/shutdown 不改变会话，offset=0，total_size=96*1024（KiB）。
  mem_set/mem_cpy/read/write 保持普通有效指针语义，不自行转换像素。

## Task 2：分配登记、范围与持有

- [ ] palloc 接受 1..32 MiB，按 4096 向上对齐；限制会话总分配 96 MiB。
  登记节点在 ioctl 前分配。按 heap ALLOC(fd返回值，0 也合法)、mmap、
  MAP_DMA_BUF 顺序执行；正常 MAP 结果必须非零且整段落在 32 位 VE
  地址范围内，且不与已登记地址重叠。分配失败返回 NULL，记录首错。
  heap ALLOC 失败释放节点；mmap 失败关闭 fd/释放节点；MAP 返回错误
  因其可能发生在内核登记后 copy_to_user 阶段，保留 mmap/fd/登记节点，
  标记 quarantine，不向 Cedar 返回指针、不盲目 UNMAP/REL。
  MAP 成功但地址无效，同样隔离，不猜测修复。
- [ ] pfree(NULL) 无操作；只有精确基址可释放，不接受内部指针。未知或
  pinned 指针记录首错、保留分配。正常执行 UNMAP_DMA_BUF 后才 munmap、
  close DMA fd、移除节点。UNMAP/munmap 失败隔离剩余资源，不自动重试；
  close fd 失败按 Linux 已消费 fd 处理，记录错误、不重试。首错后仍允许
  对其他已知、未隔离、未 pinned 分配执行正常释放，不能再新分配。
- [ ] ve/cpu 虚实地址转换对已知非隔离分配做范围查找，支持内部偏移，
  检查溢出；未知返回 NULL。使用 uintptr_t 比较范围，不比较无关 C 指针。
  flush_cache 只允许非负且在单一分配内的范围；0 字节不发 ioctl；
  实际使用厂商 cache_range 的 start/end 地址，不对整个多 MiB SBM
  反复做 DMA_BUF_SYNC。无效/失败记录首错，void 回调不能伪装可返回错误。
- [ ] describe 给出基址、整个分配大小、输入指针偏移、借用 fd、VE 基址。
  fd 不能由调用者关闭；描述符不是新引用。pin 在同一分配最多一个，
  成功分配进程内不回绕 token；满/隔离/首错/未知失败不改输出。
  unpin 验证 token、基址、fd，允许在首错后解除正确 pin；重复/旧 token
  失败且不改变状态。unpin 不释放分配，不代替 ReturnPicture，不证明
  VPU 不会重写图片；正常调用者仍须保留 CedarC 图片直到真实显示退役。
- [ ] status 给出锁内一致快照。quarantine 计入 allocations/live_bytes；
  pin/token 不是显示完成证据。debug 输出有界、不访问设备；get_fd_by_vir
  只返回已知分配的借用 fd，否则 -1。

## Task 3：真实逻辑的离线故障验证

- [ ] 只包装 open/read/close/fstat/uname/sysconf/ioctl/mmap/munmap 等
  外部系统调用；测试适配器真实登记和状态，不模拟成功的高层 allocator。
  每个独立故障用子进程隔离不可恢复的首错/资源保留状态，父进程等待
  明确退出码。正常成功路径必须零分配、零引用、关闭匹配，不能以
  子进程退出替代正常清理的检查。隔离场景明确断言资源被保留。
- [ ] 覆盖错误身份、重复 begin、未 begin 的 open/alloc、open2 引用、
  heap/VE open 失败、ENGINE_REQ/REL 失败、fd0、4K 对齐、32/96 MiB
  边界、ALLOC/mmap/MAP/UNMAP/munmap/close 失败及准确调用顺序。
  覆盖地址边界/溢出/重叠、内部指针范围、flush 零/越界/失败、
  pinned pfree/旧 pin/重复 unpin、残留引用/分配阻止 end、健康重连。
  所有 CHECK 在 NDEBUG 下有效，进程级 alarm 限时；不运行真实设备 ioctl。
- [ ] Makefile 增加 test-memory，需显式 K2B_CEDARC_HEADERS 指向外部
  include 目录和 K2B_VENDOR_CEDAR_HEADERS 指向内核 drivers/media/cedar-ve。
  两者缺失即使有缓存也失败；不影响现有 test/test-au/test-queue/test-disp。
  cedar_uapi 包含实际头文件并 C99 编译检查 map=8/cache=16 字节、
  phy_addr 偏移=4、命令 0x206/207/504/505/506；项目 heap ALLOC=_IO('C',0x42)。

```sh
make -B -f tests/k2b/Makefile test-memory CC=cc \
  BUILD_DIR=build/k2b-memory \
  K2B_CEDARC_HEADERS=/mnt/f/temp/projects/cedarx_test/libcedarc-tina/include \
  K2B_VENDOR_CEDAR_HEADERS=/mnt/f/work/source/aw-image-build/source/kernel/linux-5.4-h618/drivers/media/cedar-ve
# 再以 CPPFLAGS=-DNDEBUG 重建运行；ASan/UBSan 检查正常路径及故障路径。
```

- [ ] 主执行者复跑测试、编译真实 AArch64 libMemAdapter.so（只构建、不加载）。
  先独立规格审查，再质量审查；按真实结果更新记录及离线同步检查点。

## 自检与范围

主执行者另补现有 `tests/k2b/check_cedar_abi.c` 的 ScMemOpsS 布局检查和
独立 `tests/k2b/check_cedar_abi.sh` 编译回归入口；不改实现者的 Makefile。
新增 fixture 通过 include_next 导入原表，以其真实字段类型构造交换
open/open2 的测试布局，不复制厂商头文件。先证明旧门禁会错误接受该变体，再固定 size=192 及 24 个
函数槽偏移，确认真实 AArch64 头文件通过而变体被拒绝。这仅锁定配套
函数表布局，不宣称预编译库行为由此获证。

内存故障可诊断且不会 abort；未知 DMA 状态明确隔离，不假称回收成功。
单实例会话仍需上层设备独占/内核干净状态检查，不声称库能检测所有外部
进程或内核异常。未复用 probe 的强制残留释放。ScMemOps 的 pin 只保护
分配不被 free，不能解决 DE 与 VPU 的画面重用；显示同步门槛保持不变。
