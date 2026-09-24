# K2B 私有 Cedar 运行时加载实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. 两个任务顺序执行，每个任务先规格审查、再质量审查。总体设计已批准，不扩大到改内核或软件解码。

**Goal:** 将固定私有 CedarC 库真实加载并可靠注册硬件 H.264，提供后续生产解码工作线程使用的类型正确函数表；在板端验证真实加载而不打开 VPU/disp。

**Architecture:** 单个进程级 loader 在互斥锁下完成前置门禁、绝对路径 dlopen、加载路径/符号来源检查和有返回值的 H.264 注册。成功后返回不可变函数表；重连复用，失败保持首次错误；所有已取得句柄保留到进程结束，没有 dlclose/reset。项目脚本在新进程启动前校验固定四 blob 的哈希并设置唯一私有库路径及精确兼容预加载。

**Tech Stack:** Linux/glibc、真实固定 CedarC 头文件、C99、pthread、libdl、CMake、外部边界包装单测、原生 strace。不新增解码器替代库或自己实现 SHA256。

## 已核实的注册依据

`videoengine.h` 声明 `typedef DecoderInterface *VDecoderCreator(VideoEngine *);`
和 `int VDecoderRegister(enum EVIDEOCODECFORMAT, char *, VDecoderCreator *, int)`。
固定 `libawh264.so` 的 `CedarPluginVDInit` 在 0xee70 取 CreateH264Decoder 的重定位地址，
0xee74 取 0x1a408 的字符串 `h264`，0xee78 置 bIsSoft=0，0xee7c 置 format=0x115，
0xee80 调用 VDecoderRegister。直接使用相同声明/参数，不调用 creator 或 void 初始化入口。
固定 videoengine 的注册成功返回0，失败返回-1；已知 OOM 未检查指针风险保留在依赖文档中。
源码 constructor 宏只定义日志功能，当前选择的源码没有调用该宏；固定 blob 的 init_array
已审计为 frame_dummy。首次真实加载仍以 strace 核对设备访问，不以此静态检查替代运行验证。

## Task 1：生产 loader 与边界单测

创建 `src/video/k2b/cedar_runtime.h`、`cedar_runtime.c` 和
`tests/k2b/test_cedar_runtime.c`；修改 `tests/k2b/Makefile` 仅增加独立 `test-runtime`。
实现者不改外部源码、四个 blob、内存组件、旧探针或其他生产文件。

- [ ] 定义真实头文件支持的接口，先写成功加载/注册、重复加载只注册一次的测试，再用可编译的拒绝桩观察失败。然后逐个增加来源/环境/失败状态边界的失败测试。

```c
#include <vdecoder.h>
#include "cedar_memory.h"
struct k2b_cedar_api {
    VideoDecoder *(*create)(void);
    void (*destroy)(VideoDecoder *);
    int (*initialize)(VideoDecoder *, VideoStreamInfo *, VConfig *);
    void (*reset)(VideoDecoder *);
    int (*decode)(VideoDecoder *, int, int, int, int64_t);
    int (*request_stream)(VideoDecoder *, int, char **, int *, char **, int *, int);
    int (*submit_stream)(VideoDecoder *, VideoStreamDataInfo *, int);
    int (*stream_frames)(VideoDecoder *, int);
    VideoPicture *(*request_picture)(VideoDecoder *, int);
    int (*return_picture)(VideoDecoder *, VideoPicture *);
    struct ScMemOpsS *(*mem_ops)(void);
    int (*memory_begin)(void);
    int (*memory_end)(void);
    int (*memory_status)(struct k2b_cedar_memory_status *);
    int (*memory_describe)(const void *, struct k2b_cedar_buffer *);
    int (*memory_pin)(const void *, struct k2b_cedar_pin *);
    int (*memory_unpin)(const struct k2b_cedar_pin *);
};
struct k2b_cedar_runtime_status { int ready, error; char detail[256]; };
int k2b_cedar_runtime_load(const char *directory, const struct k2b_cedar_api **out);
int k2b_cedar_runtime_status(struct k2b_cedar_runtime_status *out);
```

- [ ] 无效参数（NULL、空、非绝对目录、NULL out）返回 EINVAL，不改变 out 或进程状态。有效参数进入首次初始化；失败记录第一个非零 errno 和具体阶段/库/符号的有界描述，不再重试。status 在锁内复制，NULL 返回 EINVAL。成功同一 canonical 目录重复调用返回同一表，另一个目录返回 EXDEV 且不破坏 ready。失败后有效调用保持首次错误。不在此组件调用任何函数表中的解码/内存函数。
- [ ] realpath 规范化根目录，并验证根目录是目录。以下十个完整路径均必须 realpath 后仍精确等于该目录下的固定文件名，且为普通文件，拒绝越界 symlink：九库 cdc_base/MemAdapter/sbm/fbm/vdecoder/VE/videoengine/awh264/vdecVcs 和 k2b_cedar54_compat。路径和诊断处理均有界，不能溢出；不要截断后继续。
- [ ] 在任何 dlopen 前检查：LD_LIBRARY_PATH 精确为 canonical 目录；LD_PRELOAD 精确为其 libk2b_cedar54_compat.so；LD_AUDIT 缺失或空；CEDAR_K2B_KERNEL54_COMPAT 精确为1。`lstat("/etc/cedarc.conf")` 只有 ENOENT 可通过，现有文件/目录/悬空 symlink 或其他错误都拒绝。此为固定受控镜像的启动限制，不擅自删除/改写全局配置。环境必须在线程启动前设置，运行时不修改。
- [ ] 用 dlsym(RTLD_DEFAULT,"ioctl")、dlerror、dladdr/realpath 验证实际 provider 为上述兼容库，不能仅相信环境。dl_iterate_phdr 在加载前拒绝九个 Cedar SONAME 对应 basename 的外部已加载对象；加载后每个必须恰有一个、都在目标目录。它是部署误用检测，不宣称抵抗恶意替换文件/并发操纵链接器。
- [ ] 为九库逐个以固定绝对路径 `dlopen(path, RTLD_NOW|RTLD_LOCAL)`，保存句柄。每个加载成功后用 dlinfo(RTLD_DI_LINKMAP) 验证自身映射来源；最终再完整检查九库集合。失败保留此前句柄，不 dlclose。不能用 RTLD_DEEPBIND 绕过兼容库；不使用目录扫描式 AddVDPlugin。
- [ ] 从 vdecoder 句柄解析头文件中以上十个视频函数，从 MemAdapter 句柄解析七个内存函数；逐个清除/检查 dlerror，非空并用 dladdr/realpath 核对实际定义库。从 videoengine 句柄取得有真实声明的 VDecoderRegister；从 awh264 句柄取得 VDecoderCreator 类型的 CreateH264Decoder，分别验证来源。函数指针转换遵循本项目 Linux/POSIX dlsym 约定，不虚构头文件结构。
- [ ] 全部前置门禁和符号解析成功后，调用 `register_fn(VIDEO_CODEC_FORMAT_H264, "h264", creator, 0)` 并要求返回0；不得把非零当成“此前大概已注册”。随后原子发布 ready/表。每个进程最多调用一次注册；函数表发布前不写 out。所有已加载句柄，包括失败后的部分加载，保持到进程退出；不提供重置/卸载接口。
- [ ] 单测只在 getenv/realpath/stat/lstat/dlopen/dlsym/dlerror/dladdr/dlinfo/dl_iterate_phdr 外部边界包装；注册是由包装 dlsym 返回的类型正确桩记录实参，不能替换高层 gate/load 函数。fork 隔离进程全局场景，真实 pthread 锁和并发首次调用验证仅注册一次。覆盖无效参数/out 不变、根路径/普通文件/越界路径、各环境项、配置存在/悬空链/异常 errno、错误 ioctl 提供者、加载前外部同名库、缺失/重复/外部加载后库、九个 dlopen 失败点、dlinfo 失败/错误路径、缺失或错误来源符号、注册失败、失败不重试、ready 重复/换目录和并发。CHECK 在 NDEBUG 下仍执行，禁止真实 Cedar dlopen 或设备访问。包装 dlclose 即调用即失败，确认没有偷偷卸载。
- [ ] Makefile 使用外部 `K2B_CEDARC_ROOT` 下真实 include/base/include/vdecoder/include（按固定头文件依赖补齐），TINA_LINUX_SUPPORT=0。缓存构建也检查必要头文件存在；独立目标不改变旧目标。普通/NDEBUG/ASan+UBSan 运行通过，记录真实场景数，提交后独立规格/质量审查。

```sh
make -B -f tests/k2b/Makefile test-runtime CC=cc BUILD_DIR=build/k2b-runtime-unit \
  K2B_CEDARC_ROOT=/mnt/f/temp/projects/cedarx_test/libcedarc-tina
# NDEBUG 换 BUILD_DIR 并加 CPPFLAGS=-DNDEBUG；ASan+UBSan 用单独目录和相应 CFLAGS/LDFLAGS。
```

## Task 2：构建、受控启动入口与真实注册验证

修改 `cmake/K2BCedarRuntime.cmake`、`tests/k2b/check_runtime_build.sh`；创建
`tools/k2b-runtime/run-private.sh`、`tests/k2b/runtime_load_check.c` 和
`tests/k2b/check_runtime_load.sh`。父执行者负责文档和板端源码同步。

- [ ] CMake 构建静态 `k2b_cedar_runtime`（上述 C 文件），带真实 common_includes/definitions、pthread/libdl、C99/PIC/Wall/Wextra/Werror，依赖现有 ABI 编译门禁。不链接任何 Cedar 库。新 `k2b_runtime_load_check` 使用它，输出到 runtime；只构建不自动执行，不改旧九库链接检查。交叉脚本增加其 ARM64/无 Cedar DT_NEEDED 检查，始终不运行目标。
- [ ] `run-private.sh RUNTIME_DIR EXECUTABLE [ARGS...]` 仅 Linux AArch64；规范化两个路径，验证普通可读库及可执行程序存在。在子 shell 的 runtime 目录下，将项目 docs/k2b-cedarc-blobs.sha256 的固定路径前缀替换为 basename 后 `sha256sum -c -`。不生成/篡改预期哈希，不下载。随后 `exec env -u LD_AUDIT -u LD_PRELOAD -u LD_LIBRARY_PATH CEDAR_K2B_KERNEL54_COMPAT=1 LD_LIBRARY_PATH="$runtime" LD_PRELOAD="$runtime/libk2b_cedar54_compat.so" K2B_CEDAR_RUNTIME_DIR="$runtime" "$executable" "$@"`。不 sudo、不加载模块、不设置全局环境，不改配置文件。
- [ ] load_check 只接收一个绝对 runtime 路径。调用真实 loader 两次，要求返回相同非空表及 ready=1/error=0，打印实际成功状态和固定 H264/hardware 注册方式。仅调用 memory_status 确认 active/references/allocations/pinned/quarantined/error 均为0；不调用 create/initialize/creator/memory_begin，不触碰 Cedar/disp。失败打印 status.detail/errno 非零退出。
- [ ] 原生回归先运行有效库目录但没有预加载/私有环境的负例，要求门禁失败且没有库加载/注册成功；再用受控脚本运行正例。正例在非 root 用户下运行，strace 记录 open/openat/ioctl，无 Cedar/disp/heap 打开、无设备 ioctl。根据实际 dl_iterate/dladdr 门禁和 RTLD_NOW 成功确认来源与符号解析，不再仅用系统 loader --list。不要故意让真实注册分配失败、耗尽 fd/内存或调用真实 creator。
- [ ] `check_runtime_load.sh PROJECT_ROOT RUNTIME_DIR` 是上述非 root/非硬件测试入口：规范化路径、清除 loader 变量，显式运行失败负例并检查指定诊断，再调用 run-private 正例。以 strace 保存可复核日志并检查打开设备和 ioctl 的缺席；若没有 strace 明确失败，不能把未跟踪写成通过。
- [ ] 本地只交叉编译和单测；板端使用 GCC11 原生构建。代码同步必须核对干净独立分支、bundle hash、--ff-only；不传交叉 runtime。板端实际加载失败先诊断，不调用硬件或自动重启。规格和质量审查后记录证据与已知限制。

## 本阶段结束后的真实目标

loader 通过仍不等于生产解码或显示完成。下一步使用这份函数表和既有有界队列/内存会话
实现单线程 Cedar 解码与图片持有，在实际硬件验证有效 NV12 像素；显示回收仍受已批准的
RCQ/帧身份/退役证据约束。不会将此阶段的进程级注册、吞吐或测试计数当作 1080p60 验收。
