# K2B CedarC 私有运行库接入记录

本页记录 2026-09-24 离线核对及热点恢复后的原生构建；运行库已经有项目内
构建入口，但尚未接通生产解码/显示路径。
已有探针能解码，不代表其测试内存实现可原样用于显示持有的生产路径。

## 固定来源

使用此前已验证的 Tina5.0 `libcedarc_v2.0` 提交
`243f2cbe84a817344d2502f4dd3d66b81338282f`，仅选择
`library/aarch64-none-linux-gnu`。本地外部源码位于
`F:/temp/projects/cedarx_test/libcedarc-tina`。

归档 `tina-243f2cbe.tar.gz` SHA256：
`ee32abb0100b6763b11d7db61d69865c43adeff07acc2be99b28edd95b85ed04`。
归档内四个预编译核心与本地展开文件的 SHA256 逐一相同，记录于
[k2b-cedarc-blobs.sha256](k2b-cedarc-blobs.sha256)。可从外部源码根目录执行：

```sh
sha256sum -c /path/to/moonlight-embedded/docs/k2b-cedarc-blobs.sha256
```

该清单只锁定四个二进制，不是全部头文件/源码或重编译产物的完整清单。
整个归档哈希仍是源快照依据。未把这些库复制到 Git 或系统目录，未作
对外分发授权判断；上游核心库许可仍需在分发前另行确认。

## ELF 依赖核对

使用本机 WSL 的 readelf 只读检查，不通过 dlopen/ldd 执行库。
以下表格只列 CedarC 内部依赖，另有 glibc 的 libc/libdl/libm 依赖。

| 预编译库 | DT_NEEDED 的 CedarC 内部依赖 |
| --- | --- |
| libVE.so | libcdc_base.so |
| libvideoengine.so | libVE.so、libcdc_base.so、libMemAdapter.so |
| libawh264.so | libVE.so、libcdc_base.so、libvideoengine.so、libvdecVcs.so、libfbm.so、libsbm.so |
| libvdecVcs.so | libVE.so、libcdc_base.so、libMemAdapter.so、libvideoengine.so |

四个库都有对应 SONAME，未包含 RPATH/RUNPATH。因此不能只把核心库
放在可执行文件旁边，就假定嵌套依赖一定从该目录解析。
后续私有启动入口须显式限定依赖目录并检查实际解析结果，沿用探针的
项目私有加载策略，不替换系统 `.so`，也不混入其他工具链目录的库。

需从配套源码/项目代码构建的组件包括 libcdc_base、libMemAdapter、
libsbm、libfbm、libvdecoder 和精确限定的 5.4 偏移兼容层。
初次离线核对时尚未接入顶层 CMake。后续 `15d0de0` 接入默认关闭的
私有运行库构建选项；仍未开放 k2b 平台，也未加入 0x804 兼容层。

## AArch64 结构 ABI 检查

`tests/k2b/check_cedar_abi.c` 是无设备访问的编译检查。仅接受 Linux
AArch64 和 `TINA_LINUX_SUPPORT=0`，检查此前二进制核对确定的：

- `sizeof(VConfig) == 224`。
- `sizeof(VideoStreamInfo) == 72`。
- `offsetof(VConfig, veOpsS) == 152`。
- `offsetof(VConfig, pVeOpsSelf) == 160`。
- `sizeof(struct ScMemOpsS) == 192`，并逐一固定全部 24 个函数槽偏移。
  只检查总大小不足以发现同样大小的回调换位。

本机 WSL archlinux 已有 `aarch64-linux-gnu-gcc 16.1.0`，从仓库根执行：

```sh
mkdir -p build/k2b-cross
cedarc_src=/mnt/f/temp/projects/cedarx_test/libcedarc-tina
aarch64-linux-gnu-gcc -std=c99 -Wall -Wextra -Werror \
  -DTINA_LINUX_SUPPORT=0 \
  -I"$cedarc_src/include" -I"$cedarc_src/base/include" \
  -I"$cedarc_src/base/include/gralloc_metadata" \
  tests/k2b/check_cedar_abi.c -o build/k2b-cross/check_cedar_abi
file build/k2b-cross/check_cedar_abi
```

实际结果：正确配置成功生成 ARM aarch64 ELF；改为宏值 1 时触发
明确 `#error` 且 VConfig 尺寸检查失败；用宿主 x86_64 cc、宏值 0 时
触发架构拒绝。这些目标 ELF 没有执行、没有传到开发板。
已有 frame/disp 配置测试也完成同编译器的 ARM64 编译与链接。

结构尺寸检查不验证所有字段语义、核心库加载、驱动 ioctl、DMA 行为或
实际解码。交叉编译也不自动证明板端 libc 可加载所有未来运行库。

内存接口扩展的编译回归入口（仍只编译，不执行）：

```sh
sh tests/k2b/check_cedar_abi.sh \
  /mnt/f/temp/projects/cedarx_test/libcedarc-tina build/k2b-cross/memory-abi
```

真实头文件通过；错误 TINA 宏被指定门禁拒绝；从真实字段类型构造、
仅交换 open/open2 槽位的负例被内存槽偏移门禁拒绝。旧版只检查 VConfig
的门禁实际会接受此换位负例，已用旧源码编译产物及 main 符号核对。
负例 fixture 不用于运行库构建，也不复制完整厂商头文件。

本次使用的 `include/sc_interface.h` SHA256：
`dd701416488520a7ffa95eef904485af059ff45f31efdd8979ba0b127e707c53`；
厂商 `drivers/media/cedar-ve/cedar_ve.h` SHA256：
`42910bf9b511239b4589fd18616606e5b33c523eef4e607a97fd25e0fb2de24b`。
这些是定位源快照的辅助信息，不替代整体归档、ABI 检查或板端运行验证。

本地另备 `F:/temp/projects/k2b-vendor-display-ve-headers-20260924.tar`，
只含实际厂商的 `include/video/sunxi_display2.h` 和
`drivers/media/cedar-ve/cedar_ve.h`，保留相对目录；未同步上板。
归档 SHA256 为
`c3e8fb59d72d72243690f73769e0d9ce9c6622cc5f14fc34823a25702fab3cc1`。
从归档流式读取的两个文件哈希已分别与开发记录和上面的原文件哈希核对。
它供后续显式外部头文件路径使用，不是替换板端系统头文件的安装包。

## 项目内内存适配器

实现提交 `5a7a751` 新增 `src/video/k2b/cedar_memory.{h,c}`，导出真实
ScMemOpsS 入口，并增加 begin/end/status/describe/pin/unpin。它使用
实际厂商 UAPI 和项目 CMA 导出器，不是仅供测试的模拟 allocator。
没有把 probe 的 abort 或残留强制释放搬入新实现。

必须由上层在创建/初始化解码器之前 begin，并在显示已退役、图片已归还、
解码器已销毁后 end。end 不能替上层退役显示。pin 只阻止 pfree，不阻止
ReturnPicture 后的 VPU 写入；正常呈现仍需要 CedarC 图片持有协议。
首错在进程内保持，不提供清除错误、强制释放或自动重新打开的接口。

新增 `test-memory` 使用真实适配器，仅包装系统调用；54 个隔离场景覆盖
正常退出、失败保留、范围/ABI/引用/pin 与线程使用。普通、NDEBUG、
ASan/UBSan 及单独的 TSan 构建均已由主执行者复跑通过。独立规格审查和后续质量审查均无
待修复项。metadata 分配失败及计数/token 极限只有代码检查，未故障注入。
故障场景的预期保留不等价于正常退出零残留，也不当作已验证的硬件恢复。

Linux 仓库根执行：

```sh
make -B -f tests/k2b/Makefile test-memory CC=cc \
  BUILD_DIR=build/k2b-memory \
  K2B_CEDARC_HEADERS=/mnt/f/temp/projects/cedarx_test/libcedarc-tina/include \
  K2B_VENDOR_CEDAR_HEADERS=/mnt/f/work/source/aw-image-build/source/kernel/linux-5.4-h618/drivers/media/cedar-ve
# 普通/NDEBUG 使用不同 BUILD_DIR；后者增加 CPPFLAGS=-DNDEBUG。
```

每次运行该目标都会对所选真实头文件和源文件做语法/ABI 检查，防止缓存
二进制掩盖换根后的不兼容头文件。父进程实际复核了缓存情况下缺少头文件
和换位 ScMemOpsS 负例均失败。改编译标志或头文件根目录仍用 `-B` 重建。

另在真实 WSL 非 K2B 主机、不包装系统调用的短程序中调用 begin，得到
ENODEV、active=0、零分配/引用。这只验证本地身份拒绝路径；没有打开
真实板卡设备，更没有验证 K2B 上的成功初始化。

已构建 `build/k2b-cross/libMemAdapter.so`，使用真实系统调用实现、
`-fPIC -shared -pthread -Wl,-z,defs,-soname,libMemAdapter.so`，产物为
ARM aarch64 ELF。readelf 确认预期 MemAdapter 和会话入口导出；本次
产物 DT_NEEDED 只有 libc，引用符号版本最高为 GLIBC_2.33（fstat）。
没有加载、安装或传到开发板；目标系统的实际可加载性仍待核对。
该构建产物 SHA256：
`3dad9ffaa9369c6ea5669e08e9031f3fd8e3a98221de83868e7925855de1ad81`。

主机日志（忽略的构建目录）：

- `build/k2b-cross/offline-memory-20260924.log`，普通/NDEBUG/ASan+UBSan
  和交叉构建/ELF 检查；SHA256
  `57315fff9581e5a2614460b450ef126f0db19302a17d02ce4a1c2ece539ddfb0`。
- `build/k2b-memory-parent/offline-memory-gates-20260924.log`，缓存头文件
  拒绝及真实非 K2B 身份拒绝；SHA256
  `918e6d9d1b0c680bdec6b63e3db1e8a68452d7ff0db11b839be56619ad75919f`。
- `build/k2b-memory-parent/offline-memory-tsan-20260924.log`，独立 TSan
  构建的 54 个场景通过，无报告；SHA256
  `a765b59304426cfa01f655762d1b0e13a3676fcc6dd0fae1ce145b1de63079a5`。

## 固定运行库的项目内构建

提交 `15d0de0` 的公共 CMake 模块校验整个 CedarC 归档、配套 cedar_ve.h
和四个 blob 的固定哈希，只在构建目录中解压和生成私有库。归档中的
MemAdapter 不参与构建，使用本项目的显式内存会话实现。
源码文件逐个列出，不自动搜索系统 Cedar 库，不下载或安装任何库。

源码 ABI 检查是五个源码库的构建依赖。`k2b_runtime_link_check` 以真实
函数引用和 `--no-as-needed --no-allow-shlib-undefined` 链接全部九个库，
包括实际解码时才需要的 H.264/Vcs 插件依赖。该程序只作链接产物，
构建和回归测试不执行它。源码库自身使用 `-z defs`。

板端独立入口（在 `/home/kickpi/projects/moonlight-embedded` 执行）：

```sh
cmake -S tools/k2b-runtime -B build/k2b-runtime-native \
  -DCMAKE_BUILD_TYPE=Release \
  -DK2B_CEDARC_ARCHIVE=/home/kickpi/projects/cedarx_test/tina-243f2cbe.tar.gz \
  -DK2B_VENDOR_CEDAR_HEADERS="$PWD/build/k2b-vendor-headers/drivers/media/cedar-ve"
cmake --build build/k2b-runtime-native --parallel 2
```

生成库位于 `build/k2b-runtime-native/runtime`，没有系统安装步骤。
`managed-source` 是从固定归档重新生成的构建中间目录，不应在其中开发；
每次 configure 会校验输入并覆盖其中的配套源码。

Moonlight 顶层的 `BUILD_K2B_CEDARC_RUNTIME` 默认 OFF；打开它只增加这些
构建目标，不表示 Moonlight 已使用它们，也不注册 `-platform k2b`。
原有视频输出选择和音频/输入依赖保留。可在顶层配置时增加：

```sh
-DBUILD_K2B_CEDARC_RUNTIME=ON \
-DK2B_CEDARC_ARCHIVE=/path/to/tina-243f2cbe.tar.gz \
-DK2B_VENDOR_CEDAR_HEADERS=/path/to/vendor/drivers/media/cedar-ve
```

本地 x86_64/WSL 交叉构建回归：

```sh
sh tests/k2b/check_runtime_build.sh \
  /mnt/f/temp/projects/cedarx_test/tina-243f2cbe.tar.gz \
  /mnt/f/work/source/aw-image-build/source/kernel/linux-5.4-h618/drivers/media/cedar-ve \
  build/k2b-runtime-tests
```

CMake 文件使用 3.6 兼容 API；上面命令及 shell 回归入口使用现代 CLI，
需要 CMake 3.13 或更新。交叉回归的“本机 cc”负例假定宿主不是 ARM64，
不能在 K2B 上原样运行整份脚本。板端用独立入口作原生构建。
主执行者独立复跑九库 ELF/SONAME/导出/ORIGIN/哈希检查及完整链接、五类
错误输入和缓存缺输入拒绝，全部通过；实现者另验证过构建目录含空格。
独立规格审查通过后完成质量审查，没有待修复项；审查结论仅覆盖构建组件。

### 交叉产物与板端原生产物不可混用

本机 GCC 16 / 新 sysroot 生成的 libcdc_base.so 引用了
`__isoc23_sscanf@GLIBC_2.38`、`__isoc23_strtol@GLIBC_2.38`；K2B 实测 glibc
为 2.35，因此这套交叉编译库没有上传或加载。板端 GCC 11.4/CMake 3.22.1
已成功原生构建全部九库与严格链接检查 ELF；所有九库的最高引用版本为
GLIBC_2.34。四个 blob 哈希未变，源码库均有正确 SONAME 与 `$ORIGIN`
RUNPATH。版本范围相容仍不等于运行时解析和设备行为已验证。

另用板端系统动态链接器的 `--list` 诊断模式（清除 LD_PRELOAD、LD_AUDIT、
LD_LIBRARY_PATH，并用 --library-path 指定私有目录）核对链接检查 ELF：
全部九个 CedarC 库都解析到 `build/k2b-runtime-native/runtime`，glibc/libm/
libdl 使用板端系统库。该诊断不进入目标 main，不作插件注册或设备初始化，
也不等价于对未来 dlopen 路径或全部运行时重定位的验证。

厂商源码警告没有隐藏。原生 Release 构建报告 CdcIonUtil 的 NULL 到
整数转换、CdcSysinfo 忽略 read 结果、iniparser 的 sprintf 潜在边界溢出，
以及 fbm/sbm/vdecoder 日志格式类型警告。新 MemAdapter 保持
`-Wall -Wextra -Werror`。这不是“整个厂商代码无警告”的构建。
配套配置解析源码默认读取 `/etc/cedarc.conf`；后续初始化前必须核查
实际配置，不能因库放在私有目录就假定配置也已隔离。
本次只读核对板端没有 `/etc/cedarc.conf`；这只是当前状态，不是未来启动
可跳过配置核查的依据。

本机保存的验证日志：

- `build/k2b-cross/runtime-build-parent-20260924.log`：主执行者交叉回归，
  SHA256 `b57a47aba9f6098cb078ca0f497abe5657a1143db61c687031aa8fe2b3d8462d`。
- `build/k2b-cross/runtime-build-native-20260924.log`：板端原生 Release 构建，
  SHA256 `0c6f49868800b2a599b9f1046c0111a9127f6687201d7a3f204f52aa4c01e177`。
- `build/k2b-cross/runtime-native-elf-baseline-20260924.log`：板端 ELF、库哈希
  及默认 OFF 的 Moonlight 重构建，SHA256
  `81475a900aa367d920618794ed46f0960abee931236c7116d93dc86b34ffa32f`。
- `build/k2b-cross/runtime-integrated-native-20260924.log`：默认视频基线配置
  加 BUILD_K2B_CEDARC_RUNTIME=ON 后完整 Moonlight 与运行库共同构建成功，
  SHA256 `66ecf1a0309e2bd40f88cd314f9ead430a3a8a3363b24b830f94f283ead47ecc`。
- `build/k2b-cross/runtime-native-loader-list-20260924.log`：系统链接器
  --list 解析路径，SHA256
  `86edd296727cb4b2661c2973d896deddc9af7df960f5ad763e515e1081d22c03`。

## 生产路径接入仍需处理

新内存组件已经由上述 CMake 入口构建，但尚未接入 CedarC 初始化包装或
显示工作线程。需要把已提供的首错传递、显示持有和停止顺序纳入统一状态，
不能因新库可以编译就绕过实际图片/显示退役门槛。

保留 ENGINE_REQ → DMA 导入 → 显式 UNMAP → ENGINE_REL 顺序，并提供
带分配大小/偏移的借用 fd 描述。网络输入要在回调返回前取得自有副本，
再经有界队列送给受控的 CedarC 线程；不让 Moonlight 链节点悬空。
已实现的 access_unit 模块只解决这一复制边界，不是完整队列或解码器。

输入队列现已另行完成离线验证，记录见开发日志；尚未接入真实解码器。
[内存会话审计](k2b-cedar-memory-lifecycle.md) 进一步明确了 void 回调的
首错传播、显式会话、失败隔离和 CedarC 初始化非事务性回滚的限制。
不能将用户态适配器自己的引用归零等同于全部厂商内核状态已经干净。

[插件加载核查](k2b-cedarc-plugin-loading.md) 已确认 AddVDPlugin 的目录扫描
和 void 注册结果问题。后续仅加载固定 H.264 插件，管理进程级句柄与
注册状态，并检查真实动态依赖来源；当前构建闭包不替代此运行时门禁。

后续 `2840c0b` / `f7e5e01` 已加入独立的 0x804 精确兼容分发、ARM64
预加载入口和原生管道 ABI 测试；命令与实际验证范围见
[兼容层记录](k2b-cedar54-compat.md)。该库不链接到九库闭包或 Moonlight，
没有系统安装；仍需未来私有启动入口显式激活。板端原生独立/集成构建
及普通 ioctl 转发已验证，真实 Cedar 正例未在本阶段重新执行。

显示持有/退役依然按 [disp 同步审计](k2b-disp-sync-audit.md) 的证据门槛
推进；不能用结构 ABI 编译通过或码流复制单测替代实际 1080p60 验收。

## 私有加载器的本地验证

`f08f389` 增加 `cedar_runtime.{h,c}`：使用真实 CedarC 类型的函数表，
核对环境、全局配置缺席、十个固定文件、实际映射和符号提供者后，才调用
有返回值的 `VDecoderRegister(H264, "h264", creator, 0)`。进程内串行初始化，
成功只注册一次；失败保留首错和已加载句柄，不重试、不卸载。此组件不调用
解码器、内存会话或 creator。注册来源和二进制初始化边界见
[插件加载核查](k2b-cedarc-plugin-loading.md)。

主执行者使用真实外部头文件重新编译运行普通、`NDEBUG`、ASan/UBSan 三组
边界测试，各为 **242 场景、0 失败**；外部边界被包装，未真实加载 Cedar。
测试包含 16 线程首次调用、九个加载失败位置、20 个符号/提供者位置及
错误保持、输出不变和禁止提前执行 API。缓存二进制在指定不存在的头文件
根目录时仍按 `required CedarC header missing: include/vdecoder.h` 拒绝。

```sh
make -B -f tests/k2b/Makefile test-runtime CC=cc \
  BUILD_DIR=build/k2b-runtime-parent \
  K2B_CEDARC_ROOT=/mnt/f/temp/projects/cedarx_test/libcedarc-tina
# NDEBUG：另用 build/k2b-runtime-parent-ndebug，增加 CPPFLAGS=-DNDEBUG。
# ASan/UBSan：另用 build/k2b-runtime-parent-asan，使用
# CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
# LDFLAGS='-fsanitize=address,undefined'。
```

原有 frame 59 项、access_unit 272 项、input_queue、compat 245 项亦复跑通过。
独立规格审查后完成独立质量审查，没有待修复项。审查仅覆盖加载器组件。
真实板端 `dlopen`/注册、受控启动脚本和构建接入仍是下一任务；不能把本节的
包装测试当成这些步骤已经完成，更不能当成 VPU/HDMI 验收。

随后以校验过 SHA256 的 Git bundle 快进同步板端 `bec2651` → `68a8ce7`，
原生 GCC 11 普通和 `NDEBUG` 各 242 场景零失败，仍是包装边界的单测。
使用既有 `build/k2b-runtime-native/managed-source` 内固定归档生成的真实
头文件，没有上传交叉编译库。启动 ID 未变，`/dev/cedar_test_heap` 仍不存在。
本机保留 `build/k2b-cross/runtime-loader-native-unit-20260924.log`，SHA256：
`7b8b42731dee86c544483794b1a1c0f8db87d6b4f76cba9a3de3a3b16f82e092`。

## 真实私有加载与注册已验证

`39cff35` 接入静态 loader、只构建不自动执行的 load-check、私有启动脚本和
非 root 原生跟踪入口；`2557b17` 补齐动态链接器特殊路径拒绝。两个入口拒绝
运行库目录中的空白、冒号、分号和美元符号，避免路径列表拆分或动态 token
展开；依据见 [ld.so 手册](https://man7.org/linux/man-pages/man8/ld.so.8.html)。
实际 shell 入口的 8 个路径负例已观察旧规则失败、新规则通过，未伪造架构。

板端以校验后的源码 bundle 快进到 `2557b17`，使用 GCC 11 原生构建。
以下命令由普通 `kickpi` 用户执行，不 sudo、不打开 VPU/disp：

```sh
cd /home/kickpi/projects/moonlight-embedded
env -u LD_AUDIT -u LD_PRELOAD -u LD_LIBRARY_PATH \
  -u CEDAR_K2B_KERNEL54_COMPAT -u K2B_CEDAR_RUNTIME_DIR \
  sh tests/k2b/check_runtime_load.sh "$PWD" "$PWD/build/k2b-runtime-native/runtime"
```

负例在 `environment LD_LIBRARY_PATH` 门禁返回 EINVAL、ready=0，跟踪中没有
Cedar 库打开。正例四个 blob 哈希通过，真实 `RTLD_NOW` 加载、符号来源检查
及有返回值的 H.264 硬件插件注册通过；两次调用返回同一表、ready=1/error=0。
唯一调用的内存 API 是 status：active/references/allocations/live_bytes/
peak_bytes/pinned/quarantined/error 全为0，没有调用 begin、creator 或解码器。
跟踪确认实际检查进程成功打开十个目标私有对象，没有设备打开或 ioctl。

原生独立构建与顶层 ON 集成目录均完成上述负/正例；默认 OFF 的 Moonlight
构建也通过。集成 load-check 的 DT_NEEDED 只有 libc 和系统动态链接器，
没有 Cedar 库。Moonlight 本身没有运行，也尚未使用新函数表或注册 k2b 平台。
启动 ID 仍为 `66ed4017-1fae-4b40-82ca-877d257b5175`，heap 设备仍不存在。

规格审查及修正复审通过；质量审查和独立原生证据复核没有 Critical/Important
问题。保留一个非阻塞限制：跟踪脚本直接匹配 strace 中的路径，带引号、反斜线
或需转义字符的目录可能被误报为缺少打开记录。本次固定 ASCII 目录不受影响；
未宣称任意 Linux 文件名均受支持。注册函数自身未检查 OOM 的依赖风险仍在。

本机证据均位于忽略的 `build/k2b-cross/`：

- `runtime-loader-pathfix-parent-20260924.log`，完整交叉与路径/输入/缓存负例，
  SHA256 `f24a85c913986f1062d55da156945624837693089ddd01360ab3befef44cb30c`。
- `runtime-loader-native-build-retry1-20260924.log`，源码同步和原生构建，
  SHA256 `ebbbb9638f18b4e6031e06558ab03e92029329addecfbcf385055d870c342cc6`。
- `runtime-loader-native-load-20260924.log`，独立目录原生加载，
  SHA256 `46c41e355d0c0f3311211987404ef0c24f2b076fd3c49d34abda64a015bb5594`。
- `runtime-loader-native-integrated-20260924.log`，OFF/ON 构建及集成目录加载，
  SHA256 `7fe9f1a6c747368de79ee9990d066dc3a6cd99d8d4842372ca880e9a11f99366`。
- `k2b-runtime-load.5QPkHn/negative.trace`，SHA256
  `9390ab540f12dc5d839676c9456cc9860beec93bc40d33e3fb89925d9c4ba63d`。
- `k2b-runtime-load.5QPkHn/positive.trace`，SHA256
  `51c8daa129c4c3e196c2759c26fdcada3552f7997c2d5745f0306f203633db70`。

两个原始运行目录 `build/k2b-runtime-load.5QPkHn`（独立）和
`build/k2b-runtime-load.Xxl77K`（集成）均留在板端；各六个日志已复制到本机
`build/k2b-cross/` 对应子目录。首次同步尝试曾在 SSH 认证前失败，未上传或
执行程序；用户重连热点后恢复。失败日志没有被当成成功构建证据。

此检查点只证明实际私有库加载/注册，不是硬件解码或显示通过。接下来使用
函数表连接输入队列、内存会话和单线程解码；显示退役证据、真实动态 NV12
和完整 1080p60 串流验收仍未完成。
