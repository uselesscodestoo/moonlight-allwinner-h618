# K2B 私有 CedarC 运行库构建实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在项目中构建固定版本 CedarC 的真实 ARM64 私有依赖闭包，为后续解码线程提供可链接库，不再依赖探针构建脚本。

**Architecture:** 公共 CMake 模块创建私有库目标；独立入口只构建运行库，Moonlight 顶层默认关闭的选项复用同一入口。CedarC 归档校验后展开在构建目录；使用项目自己的 cedar_memory.c。编译 ABI 检查、完整链接检查和 ELF 检查均不执行目标代码。

**Tech Stack:** CMake 3.6 兼容语法、Linux AArch64 GCC、固定外部 CedarC 归档、实际厂商 cedar_ve.h。

沿用已批准整体设计与现有独立 worktree；板卡离线，不连接、安装、推送或改内核。此计划只完成真实运行库构建接入，不表示解码/显示后端或 1080p60 验收完成。0x804 兼容层与运行时加载自检另行接入，不复制未经生产审查的 probe interposer。

## Task 1：可重复的私有运行库构建

**Files:**
- Create: `cmake/K2BCedarRuntime.cmake`，验证输入、ABI 与真实库目标。
- Create: `tools/k2b-runtime/CMakeLists.txt`，无需 Moonlight 音频/网络依赖的构建入口。
- Create: `tests/k2b/check_runtime_build.sh`，独立构建和拒绝路径回归，不执行 ARM64 程序。
- Create: `tests/k2b/runtime_link_check.c`，真实库完整链接检查入口。
- Modify: `CMakeLists.txt`，仅增加默认 OFF 的 `BUILD_K2B_CEDARC_RUNTIME` 与对应子目录。
- 主执行者修改运行库记录与开发日志，实现者不改文档。

- [ ] 先写构建回归测试，运行并确认缺少构建入口导致预期失败，然后实现；配置/链接失败不得伪装通过。
- [ ] 独立入口支持下列配置并在构建目录内产出 9 个库：

```sh
cmake -S tools/k2b-runtime -B build/k2b-runtime \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DK2B_CEDARC_ARCHIVE=/mnt/f/temp/projects/cedarx_test/tina-243f2cbe.tar.gz \
  -DK2B_VENDOR_CEDAR_HEADERS=/mnt/f/work/source/aw-image-build/source/kernel/linux-5.4-h618/drivers/media/cedar-ve
cmake --build build/k2b-runtime --parallel 4
```

无 try_run、ldd、dlopen、下载或系统安装。仅 Linux AArch64、64 位构建；用编译检查确认真实编译器，不只信 CMAKE_SYSTEM_PROCESSOR。本地 CMake 4 如有旧策略警告可声明兼容策略范围，不能改变顶层默认构建行为。

- [ ] K2B_CEDARC_ARCHIVE 必须存在且 SHA256 精确为 `ee32abb0100b6763b11d7db61d69865c43adeff07acc2be99b28edd95b85ed04`；不能允许替换期望哈希。校验后在当前 binary dir 下专用 managed-source 目录用 cmake -E tar 解压，检查固定顶层目录 `libcedarc_v2.0-243f2cbe84a817344d2502f4dd3d66b81338282f-243f2cbe84a817344d2502f4dd3d66b81338282f`；此目录是可重建的托管输出，不能改外部源码。每次 configure 校验，不因 cache 跳过。
- [ ] 显式要求 K2B_VENDOR_CEDAR_HEADERS/cedar_ve.h，SHA256 为 `42910bf9b511239b4589fd18616606e5b33c523eef4e607a97fd25e0fb2de24b`；缺失/不符报清晰 FATAL_ERROR。固定本板源版本，不自动猜测系统头文件。
- [ ] 四个 blob 仅取解压树 `library/aarch64-none-linux-gnu`，用现有 `docs/k2b-cedarc-blobs.sha256` 再次核对；复制到 binary dir 的独立 runtime 目录，不进 Git，不安装，不修改 ELF。使用 imported targets 和准确路径，不从系统查找同名 Cedar 库。
- [ ] 源码库只编译明确文件列表：cdc_base 的 base 七个 .c、cdcIniparser 三个 .c、filesink 三个 .c；sbm 六个 .c；fbm/fbm.c；vdecoder/pixel_format.c 与 vdecoder.c；MemAdapter 使用本项目 src/video/k2b/cedar_memory.c，不使用归档 memory 实现。不 glob 自动收纳新源码。
- [ ] 配套 include 根：include、base/include、base/include/gralloc_metadata、base/filesink、base/filesink/include、vdecoder、vdecoder/include、vdecoder/aftertreatment。宏与已验证探针配套：TINA_LINUX_SUPPORT=0、_GNU_SOURCE、CEDAR_TINA、CONF_USE_IOMMU=1、CONF_KERNEL_VERSION_5_4、CONF_VE_ENCODER_VERSION_1、ENABLE_AFTERTREATMENT=0。仅作用于新目标，不污染 Moonlight 全局。
- [ ] 全部源码 shared libs 使用 PIC、正确 SONAME、私有 runtime 输出目录及 $ORIGIN RPATH；MemAdapter 以 -Wall -Wextra -Werror 编译，其他厂商源码保留诊断不强行全局忽略。可以针对实际验证的旧源码/新 GCC 兼容诊断做最小目标级处理，必须报告原因。新库链接要求无自身未解析强符号；外部 blob 的完整闭包由独立链接检查覆盖，不能用 --allow-shlib-undefined 跳过。
- [ ] 复用 tests/k2b/check_cedar_abi.c 为只编译目标并使源码库依赖它。TINA=0、VConfig/ScMemOps ABI 门禁每次正常构建生效。
- [ ] runtime_link_check.c 使用真实 vdecoder.h/cedar_memory.h，引用 CreateVideoDecoder、InitializeVideoDecoder、DestroyVideoDecoder、RequestVideoStreamBuffer、SubmitVideoStreamData、DecodeVideoStream、RequestPicture、ReturnPicture、MemAdapterGetOpsS、k2b_cedar_memory_begin/end。以 --no-as-needed 和 --no-allow-shlib-undefined 链接所有 9 个库，覆盖 dlopen 才会出现的 libawh264/libvdecVcs；不运行该程序。若真实 blob 闭包不闭合，报告具体符号，不提供虚假 stub。
- [ ] 默认 OFF 的顶层选项不得注册 HAVE_K2B、开放 -platform k2b、改变原平台选择或要求 CedarC 输入；ON 只增加真实运行库目标。顶层已存在的依赖与 no-video 检查保持原意。
- [ ] 测试检查所有 9 个文件为 ARM64 ELF、SONAME 正确、期望导出、编译库 RPATH/RUNPATH 含 $ORIGIN、blob 哈希相同、完整链接检查产物存在；不把 ELF 检查宣称为运行时加载证明。
- [ ] 测试拒绝缺归档、错误归档哈希、缺/错误厂商头文件、假称 aarch64 的本机 cc，且核对具体失败诊断。缓存重配置缺输入也必须拒绝。测试只使用独立临时/指定构建子目录，不清理外部源码或工作树，不触及设备。

测试入口：

```sh
sh tests/k2b/check_runtime_build.sh \
  /mnt/f/temp/projects/cedarx_test/tina-243f2cbe.tar.gz \
  /mnt/f/work/source/aw-image-build/source/kernel/linux-5.4-h618/drivers/media/cedar-ve \
  build/k2b-runtime-tests
```

- [ ] 实现者自检/提交；独立规格审查通过后质量审查；主执行者重新运行真实构建与拒绝回归。记录产物 libc 要求、编译警告和未验证边界。

## 主执行者并行核查与后续

- [ ] 核查固定 libvideoengine 的 AddVDPlugin/CedarPluginVDInit 路径，确认后续应怎样限定 H.264 插件及其依赖，记录源文件/ELF 依据，不运行 blob。
- [ ] 记录当前只是构建闭包，0x804 兼容、运行时解析检查、解码工作线程、显示退役与上板验证仍未完成；保持完整目标 active。

## 自检

所有修改局限新 runtime 构建入口与关闭式顶层选项。未更改已验内存层、压缩队列、原后端或独立探针。固定归档比仅四个 blob 哈希更完整地约束源码；受托管的展开目录不是外部开发树。链接闭包测试不证明 glibc 在板端相容或动态插件选择正确，因此必须保留独立运行时加载检查。
