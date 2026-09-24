# K2B Cedar 5.4 精确 ioctl 兼容层实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 提供可用于本项目私有启动入口的 ARM64 兼容库，仅修正固定 K2B 内核缺少的 VE 寄存器零偏移查询，保持所有其他 ioctl 的真实参数和结果。

**Architecture:** ARM64 汇编入口将 ioctl 的 x0/x1/x2 原样尾调用到隐藏的固定三参数 C 函数；不通过 va_arg 猜参数是否存在/是什么类型。C 函数仅在 request=0x804、显式 opt-in、严格身份匹配时返回 0，其他情况调用真实 syscall(SYS_ioctl, fd, request, raw_arg)。只校验身份，不打开 Cedar，不获取/释放 ENGINE。

**Tech Stack:** C99、Linux AArch64 GNU 汇编、ELF LD_PRELOAD、CMake 私有库、链接包装单测、板端管道 ioctl 回归。

整体设计已获批准，当前网络恢复。此实现不修改内核、外部 CedarC 源码、blob 或独立探针。没有真实解码/显示测试；不开 /dev/cedar_dev 或 /dev/disp，不加载模块，不触发厂商 close 副作用。完整 1080p60 目标保持不变。

## 依据与文件边界

固定 libVE.so 的 veEnvGetIcVersion 在 0x2c48..0x2c4c 发出 request=0x804、arg=0，随后将返回值作为寄存器基址偏移使用。保存的同代 BSP `inspect/tina-cedar_ve_uapi.h` 将 0x800 起的第 5 个命令定义为 GET_VE_TOP_REG_OFFSET；`inspect/tina-cedar_ve.c:718` 返回 ve_top_reg_offset。K2B 5.4 cedardev_mmap 直接映射 MACC 起点，且没有这个查询分支。因此兼容返回零不伪造硬件 ID、码流、像素或完成事件。

创建：
- `src/video/k2b/cedar54_compat.c`：隐藏的固定参数分发、身份校验与 syscall 转发。
- `src/video/k2b/cedar54_ioctl_aarch64.S`：唯一公开 ioctl 符号的保寄存器尾调用入口。
- `tests/k2b/test_cedar54_compat.c`：只包装系统调用/环境读取，运行真实分发逻辑。
- `tests/k2b/test_cedar54_preload.c`：只用管道/fcntl/dladdr 的真实 ioctl ABI 测试。
- `tests/k2b/check_cedar54_preload.sh`：运行上述 ABI 测试的无硬件入口。

修改：
- `tests/k2b/Makefile`：无外部 Cedar 依赖的 `test-compat`，默认目标不变。
- `cmake/K2BCedarRuntime.cmake`：新增 libk2b_cedar54_compat.so 与 preload 测试可执行文件，只构建不默认执行。
- `tools/k2b-runtime/CMakeLists.txt`：在文件作用域启用 ASM；不让默认 OFF 的主工程依赖 ASM。
- `tests/k2b/check_runtime_build.sh`：额外检查兼容库 ARM64/SONAME/ioctl 导出与隐藏分发、不执行目标程序，保留原九库闭包验证。

主执行者负责文档与源码证据核对，实现者只改以上九个文件。

## Task 1：明确匹配与 errno 透明的分发

- [x] 先写能编译的 ENOSYS 最小桩及成功兼容/正常转发测试，观察断言失败，再实现真实逻辑。新增边界先测试失败，不能仅运行事后测试。
- [x] 隐藏入口原型为 `int k2b_cedar54_ioctl_dispatch(int fd, unsigned long request, unsigned long raw_arg)`。仅 request 精确等于 0x804UL 时读取环境和身份；其他 request 不访问 model、stat、uname 或 getenv，直接转发原始参数。
- [x] `CEDAR_K2B_KERNEL54_COMPAT` 精确为字符串 `1` 才允许兼容；未设置、空串、0、01、1x 都转发。调用者必须在线程启动前设置环境，兼容层不改环境、不缓存身份结果、不支持 signal-handler。
- [x] uname release=5.4.125、machine=aarch64；fstat(fd) 与 stat(/dev/cedar_dev) 均成功、都是字符设备、st_rdev 相同。不能只检查调用者 fd 的 mode 而不检查目标 mode。不打开 Cedar 设备；身份校验失败仅转发，不能自创成功或伪造设备错误。
- [x] /proc/device-tree/model 以 O_RDONLY|O_CLOEXEC 打开，完整读取到 EOF 或比合法长度更长的边界。只接受精确 KICKPI K2B（可带单个末尾 NUL），拒绝前缀、额外 NUL/字符、嵌入 NUL、过长/短、读取或关闭失败。允许有效 fd=0。循环处理短读；不把未看到完整内容的缓冲区当成字符串成功。
- [x] 函数入口保存 errno。身份匹配时返回0并还原入口 errno；不匹配时，在 syscall 前还原入口 errno，让真实 syscall 决定结果/errno。syscall 成功不得泄漏身份探测的 errno，失败必须保留 syscall 的 errno。转发不重试、不改 fd/request/raw_arg，不强制关闭任何调用者 fd。
- [x] 单测以链接 --wrap 包装 getenv/uname/stat/fstat/open/read/close/syscall，不能把高层判断函数替换成模拟成功。覆盖所有上述条件、模型短读/fd0/open-read-close 错误、合法模型有无 NUL、目标普通文件、st_rdev 不同、无参数请求的任意 raw_arg 位模式、指针/标量位模式、request 高位、负 fd、syscall 正/0/-1 结果和 errno。assert 在 NDEBUG 下也有效，不访问实际设备。

```sh
make -B -f tests/k2b/Makefile test-compat CC=cc BUILD_DIR=build/k2b-compat
make -B -f tests/k2b/Makefile test-compat CC=cc CPPFLAGS=-DNDEBUG BUILD_DIR=build/k2b-compat-ndebug
```

## Task 2：真实 ARM64 入口与私有构建

- [x] 汇编只支持 Linux AArch64 LP64，其他架构 #error；导出 ioctl，标记正确函数类型/size/非执行栈。尾调用固定分发函数，不在 C 中 va_arg，也不把传入寄存器变成其他参数。可添加 BTI 兼容提示与 CFI，不引入新依赖。隐藏 helper，不公开第二个绕过验证的公共 API。
- [x] CMake 在独立入口文件作用域启用 ASM；兼容库为 PIC、正确 SONAME、-z defs，C 源使用 -Wall -Wextra -Werror。无 Cedar 库依赖，不链接进 Moonlight 本体或原九库闭包；未来仅由显式私有启动入口 preload。默认 OFF 主构建无新增 ASM 需求。
- [x] 增加 preload 测试编译目标，链接 libc/libdl，无 Cedar 依赖，不在构建时运行。测试检查 dladdr(ioctl) 的实际提供库：--baseline 要确认 libc，显式库路径参数要与实际 provider 的 realpath 相同，不能因 LD_PRELOAD 被忽略而误通过。
- [x] 真实测试仅创建管道并写少量数据，验证 FIONREAD 指针、FIONBIO、FIOCLEX/FIONCLEX 双参数调用、带无关 raw_arg 的无参数请求、无效 fd 与不支持请求的错误、errno。不得 open Cedar/disp、请求时钟/寄存器或加载模块。资源结束时关闭。
- [x] preload 脚本参数 LIB TEST，使用绝对真实路径，清除外部 LD_PRELOAD/LD_AUDIT/LD_LIBRARY_PATH，运行 baseline、兼容 opt-in=0 和 opt-in=1 的 ABI 测试。它只支持原生 Linux AArch64；交叉构建脚本不调用它。先在板端观察“预加载库尚不存在/不是提供者”测试失败，再添加/验证真实汇编入口；不能使用另一个会假返回 ioctl 的测试共享库绕过。

```sh
sh tests/k2b/check_cedar54_preload.sh \
  /absolute/build/runtime/libk2b_cedar54_compat.so \
  /absolute/build/runtime/k2b_cedar54_preload_test
```

- [x] 交叉回归仍验证原九库，另检查兼容库为 ARM64、SONAME、ioctl 导出而 helper 不导出，且无 Cedar DT_NEEDED。不得新增 --allow-shlib-undefined。
- [x] 主执行者复跑本地普通/NDEBUG/ASan+UBSan 分发测试和板端原生 ABI 测试，核对最终反汇编。独立规格审查通过后质量审查，修复后再复核。

## 验证与风险边界

生产匹配正例由包装系统调用验证，真实 AArch64 ABI 由管道验证；不把它们写作新兼容库已在 VPU 上跑通。板端硬件正例必须等内存会话、加载注册和初始化失败策略接通后测试，不能打开 Cedar 再立即 close 去探测偏移。
该门禁只识别板型、版本与设备，不证明同版本内核从未被替换。对其他 ioctl 的转发使用 Linux 原生 syscall，不保持外部其他 interposer 的自定义行为；私有启动入口不得与不明预加载库混用。

## 自检

沿用批准路线的精确兼容，不扩大到内核修改或模拟解码。固定三参数 helper 是生产汇编入口的实现，不是测试专用 API；测试只截断真实外部边界。接口的第三寄存器是否来自 C 变参由汇编层隔离，无需猜 C 参数存在性。板端测试的设备范围严格限于管道。
