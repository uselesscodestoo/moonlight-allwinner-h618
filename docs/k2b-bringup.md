# K2B 独立后端开发记录

## 当前状态

分支 `k2b-cedarc-disp`，基于 `h618-egl-download` 的 `4e870a2` 独立维护。
没有上游 PR 或远程推送。板端仓库为 `/home/kickpi/projects/moonlight-embedded`，
本地工作树为 `F:/temp/moonlight-embedded/.worktrees/k2b-cedarc-disp`。

已完成构建准备、严格 NV12 布局契约、厂商 disp 配置转换、压缩输入复制/队列及独立 CedarC 内存组件，**尚未接入生产解码/显示后端，
也没有达到实际 1080p60 串流验收**。当前二进制仍是原有 SDL 后端构建基线，
只编译、未运行串流，不能把它当作 K2B 的最终输出路径。

## 2026-09-24 构建环境

KICKPI K2B，Linux 5.4.125，AArch64，GCC 11.4.0，CMake 3.22.1，Git 2.34.1。
板端安装了 48 个新增构建/开发依赖（约 74.4 MB），0 升级、0 移除；
未更换内核。安装入口：

```sh
sudo apt-get --no-install-recommends --no-upgrade install \
  git cmake pkg-config libevdev-dev libudev-dev libopus-dev libssl-dev \
  libcurl4-openssl-dev libexpat1-dev libasound2-dev libavahi-client-dev \
  uuid-dev libsdl2-dev
```

SDL 开发包仅用于验证原有构建，附带的图形开发库不表示板卡具有 Mali
硬件渲染能力。`dpkg --audit` 未报告未完成的包配置。

固定子模块：

- moonlight-common-c：`b126e481a195fdc7152d211def17190e3434bcce`
- SDL_GameControllerDB：`28a856f2b92da8891b161acd0abd64fbf4445d97`
- enet：`dea6fb5414b180908b58c0293c831105b5d124dd`

Windows 主机使用 Git 自带 Bash 并设置**进程内** PATH 后完成递归初始化，
没有修改系统 PATH。板端父仓库由本地 Git bundle 克隆，子模块源码从
核对哈希后的固定快照解包，不含主机 `.git` 路径。子模块在板端未建立
各自的 Git 元数据；不要将其描述为板端已执行 `submodule update`。

首次 bundle `k2b-foundation-87d0f84.bundle` SHA256：
`29247e4f1863879cf53a229c0e758616aeac3c41bf0786b88d712eaf7112fdcb`。
子模块归档 `k2b-pinned-submodules-20260924.tar` SHA256：
`bde57e485b6076e9be56e74a3c21cf1375f336f70f61ebfcb0ae341245213309`。
后续提交通过新的 bundle 和 `merge --ff-only FETCH_HEAD` 同步；不覆盖板端修改。

## 构建基线

在板端仓库根目录：

```sh
cmake -S . -B build/baseline -DENABLE_SDL=ON -DENABLE_X11=OFF \
  -DENABLE_CEC=OFF -DENABLE_PULSE=OFF
cmake --build build/baseline -j2
```

在 `87d0f84`（相对原分支只有文档变更）完成构建，并 clean-first 重建，
两次均成功链接 `build/baseline/moonlight`。`file` 确认产物为 ARM aarch64。
没有执行该二进制，也没有设置系统安装或自启动。

证据保存于板端 `/home/kickpi/projects/k2b-build-artifacts/`：
`baseline-configure.log`、`baseline-build.log`。

## 布局契约及单测

`src/video/k2b/frame.h` 描述借用的 DMA-BUF fd、字节步长、平面偏移、
分配大小、存储高、有效裁剪以及色彩矩阵/范围。`frame.c` 验证单 fd
线性 NV12，支持 1920×1088 存储中裁出 1920×1080，也支持合法的额外步长。
拒绝未知格式/颜色、非零 Y 起点、错误 UV 偏移、不足分配及越界/奇数裁剪。

这只是前置布局检查：不验证 fd 确实存在，不打开设备，不读取像素，
不证明色彩内容正确，不代表帧已显示或已经可以 ReturnPicture。

仓库根目录执行：

```sh
make -f tests/k2b/Makefile test
make -B -f tests/k2b/Makefile test CPPFLAGS=-DNDEBUG
gcc -std=c99 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc/video/k2b \
  tests/k2b/test_frame.c src/video/k2b/frame.c \
  -o build/k2b-tests/test_frame_sanitize
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build/k2b-tests/test_frame_sanitize
```

编译选项变化时用 `-B` 重建；普通 make 不跟踪命令行标志的变化。

在实现提交 `acfd079` 上：主机普通/NDEBUG 各 59 项通过；板端普通、
NDEBUG、ASan/UBSan 各 59 项通过。测试驱动过程先以拒绝桩得到有效输入
失败，再以全接受桩确认 47 项非法输入失败，最后实现全部约束。
`5e8bebb` 仅补充单位/所有权注释，主机测试再次通过。
独立规格审查与质量审查通过；没有用这些单测代替硬件验收。

板端日志：`frame-native.log`、`frame-ndebug.log`、`frame-sanitize.log`，
均在上述 `k2b-build-artifacts` 目录。测试后 DMA-BUF 为 0 个、0 字节。

## 下一阶段

将固定 CedarC 运行库、内存接口与生产后端连接；首先落实画面布局到
disp 配置的转换和可观测的缓冲区退役，不凭 ioctl 成功或固定等待时间
归还解码帧。随后才开展短动态显示、真实串流与长时间稳定性验收。

原有 `/home/kickpi/projects/cedarx_test`、`disp_test` 保持独立，不覆盖。
厂商死机/异常退出后的资源清理限制仍然存在，本阶段未宣称解决。

## 网络变更检查点

用户随后移除了手机热点，要求离线继续。已停止 SSH/上板操作。
板端最后同步提交为 `acfd079`；此后的本地提交待网络恢复后快进同步。
上述三组测试已有成功返回，但日志回传未完成，本地不能声称已备份这些
完整日志。网络恢复后先核对启动 ID、Git 状态和遗留进程，再同步和测试。

## 离线推进：disp 配置与同步审计

实现提交 `7f89499` 增加纯配置转换函数 `k2b_disp_config_prepare()`。
使用显式指定的厂商 `include/video/sunxi_display2.h`，未复制完整厂商
头文件或手写替代 ABI。合法的 NV12 帧转换到 channel 0 / layer 0，
使用 DMA-BUF fd，区分存储高和有效裁剪，覆盖 BT.601/709 全/限幅。
失败时不修改输出，不执行 ioctl，不转移 fd 所有权。

厂商头文件 SHA256：
`f573cf66d2aee34373dd4b46c8d16aaaad4d3fd4264caab22abb8f2664a570f4`。

主机普通及 `-DNDEBUG` 各通过 59 项布局检查、396 项配置检查；
有效输入先在拒绝桩上出现 8 个失败，再实现转换。
缺少/错误的 `K2B_VENDOR_HEADERS` 时，即使已有缓存产物也明确失败。
原来的 `test` 目标仍无需厂商头文件。主机命令（MSYS2 Bash）：

```sh
export PATH=/ucrt64/bin:/usr/bin:$PATH
mkdir -p build/k2b-tests/tmp
export TMPDIR="$PWD/build/k2b-tests/tmp"
export TMP="$TMPDIR" TEMP="$TMPDIR"
make -B -f tests/k2b/Makefile test test-disp CC=/ucrt64/bin/gcc \
  K2B_VENDOR_HEADERS=F:/work/source/aw-image-build/source/kernel/linux-5.4-h618/include
# 再用 CPPFLAGS=-DNDEBUG 强制重建、运行相同目标。
```

更换编译选项或 `K2B_VENDOR_HEADERS` 根目录时必须用 `-B`；当前 Makefile
会跟踪所选头文件的时间戳，但不会把根目录参数变化编码为构建依赖。
本地普通/NDEBUG 重跑日志位于 `build/k2b-tests/offline-host-20260924.log`
（忽略的构建目录，不纳入 Git）；这不是尚未回传的板端测试日志。

Windows 主机测试不证明 AArch64 的实际 ioctl ABI 或硬件显示正确；
这批代码尚未同步上板，也没有接入 Moonlight 主构建。

[同步接口审计](k2b-disp-sync-audit.md) 记录了源码中已有的 composer
release fence，以及它未与 RCQ 事务/指定缓冲区绑定的限制。该文区分
源码事实、时序风险推演和待上板验证项；尚未据此编写帧回收路径。
需要先核实运行内核配置，再关联 fence、RCQ 完成、实际扫描地址与
DMA-BUF 退役。未改变内核、桌面、自启动或原有后端。

## 离线 Linux / AArch64 验证补充

本机 WSL archlinux 提供原生 cc 及 `aarch64-linux-gnu-gcc 16.1.0`。
在独立 `build/k2b-linux` 目录对 frame/disp 代码执行 ASan+UBSan，分别
通过 59 / 396 项检查；没有连接开发板。相同源码也成功交叉编译、链接
成 ARM aarch64 ELF，产物位于 `build/k2b-cross`，未执行这些目标程序。

[CedarC 私有运行库接入记录](k2b-cedarc-runtime.md) 包含固定核心库哈希、
ELF 依赖、真实头文件的 AArch64 结构 ABI 检查及剩余生产接入限制。
ABI 正例编译通过；错误 TINA 宏或 x86_64 目标均被编译期拒绝。
这不等价于 Moonlight 已加载新运行库，也不等价于板端通过新后端回归。

## Moonlight 完整码流帧边界

实现提交 `7f6d0a6` 增加 `src/video/k2b/access_unit.{h,c}`，直接使用
固定 moonlight-common-c 的 `DECODE_UNIT` / `LENTRY`。
`k2b_access_unit_copy()` 验证整条碎片链后，才复制到调用者提供的存储。
它保留 SPS/PPS/图像片的原始压缩字节、帧号与时间信息，复制成功后不再
依赖源链存活；不是解码像素复制，也不进行 NAL 解析或格式转换。

当前输入上限 4 MiB，限定声明为 H.264/SDR 路径可接受的容器类型与
Rec.601/709。非法输入返回 INVALID；完整验证后发现容量不足则返回
NO_SPACE。两种失败都不修改目标存储或输出元数据，回调层后续负责
丢帧/关键帧策略。源指针可读、目标实际容量及稳定/不重叠由调用者保证。

拒绝桩阶段 189 项已执行检查中 11 项按预期失败（成功复制与容量错误
尚未实现）；真实实现运行 272 项通过。Windows 与 WSL Linux 的普通/
NDEBUG 构建均通过，Linux ASan/UBSan 同样通过；AArch64 只编译未执行。
这不是已经接好的线程队列、CedarC 解码器或 Moonlight 视频回调。

从仓库根运行（Linux，无厂商头文件即可测码流复制）：

```sh
make -B -f tests/k2b/Makefile test-au CC=cc BUILD_DIR=build/k2b-linux
make -B -f tests/k2b/Makefile test-au CC=cc \
  BUILD_DIR=build/k2b-linux-ndebug CPPFLAGS=-DNDEBUG
cc -std=c99 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc/video/k2b -Ithird_party/moonlight-common-c/src \
  tests/k2b/test_access_unit.c src/video/k2b/access_unit.c \
  -o build/k2b-linux/test_access_unit_sanitize
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build/k2b-linux/test_access_unit_sanitize
```

在该提交上重新运行的 Linux 布局/码流/配置测试分别为 59/272/396 项，
普通、NDEBUG、ASan/UBSan 都通过。日志位于本地忽略目录
`build/k2b-linux/offline-input-20260924.log`，SHA256：
`9eda7d2b3356d7d67f1fb8d4f6906b0a2e1bf8638ec7a481773ad4c361846707`。
它不包含板端运行证据；开发板最后同步位置仍是此前记录的 `acfd079`。

## 离线有界输入队列

实现提交 `c3485eb`；独立规格审查、随后质量审查均无待修复项。
token 耗尽和资源初始化失败分支经源码检查，尚未做故障注入覆盖。

`src/video/k2b/input_queue.{h,c}` 在创建时分配固定槽位，最多 8 槽、
每槽最多 4 MiB。这个上限不是板端调优结果。push 不等待空位，也不在
每帧分配内存；mutex 仍可能短暂等待，所以不能称为无锁或 wait-free。
数据复制使用真实 access_unit 模块，不在回调返回后依赖 common-c 源链。

单消费者借用的队头仍计入占用，归还前不能被覆盖。stop/discard 丢弃
待处理输入，但保留正在借用的帧；stop 唤醒等待线程。销毁要求调用者
先停止并 join/quiesce 其他调用，不是并发取消接口。队列没有实现 IDR
恢复，也不代表 CedarC 图片或 DMA-BUF 已可回收。

[输入生命周期审计](k2b-input-lifecycle.md) 记录了后续回调接入必须处理
的 stop/cleanup 次序、异步 IDR 请求差异及部分初始化失败路径。该审计
已独立对照固定 common-c 和 CedarC 源码复核。

测试先以拒绝桩观察 create 成功路径失败，再完成实现。Linux 普通与
NDEBUG、ASan+UBSan、ThreadSanitizer 均实际运行通过；含两种真实条件
等待唤醒及 12,000 帧 SPSC 压力测试，压力期间并发读取统计。TSan 本次
可以运行，不是仅编译成功；仍不等价于证明所有可能线程调度无误。
AArch64 测试程序已交叉编译/链接为 ARM64 ELF，未执行、未同步上板。

Linux 仓库根执行：

```sh
make -B -f tests/k2b/Makefile test-queue CC=cc BUILD_DIR=build/k2b-linux-parent
make -B -f tests/k2b/Makefile test-queue CC=cc \
  BUILD_DIR=build/k2b-linux-parent-ndebug CPPFLAGS=-DNDEBUG
cc -std=c99 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc/video/k2b -Ithird_party/moonlight-common-c/src \
  tests/k2b/test_input_queue.c src/video/k2b/input_queue.c \
  src/video/k2b/access_unit.c -o build/k2b-linux-parent/test_input_queue_sanitize
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build/k2b-linux-parent/test_input_queue_sanitize
# TSan 单独编译运行，不与 ASan 同时启用。
cc -std=c99 -O1 -g -Wall -Wextra -Werror -pthread \
  -fsanitize=thread -fno-omit-frame-pointer \
  -Isrc/video/k2b -Ithird_party/moonlight-common-c/src \
  tests/k2b/test_input_queue.c src/video/k2b/input_queue.c \
  src/video/k2b/access_unit.c -o build/k2b-linux-parent/test_input_queue_tsan
TSAN_OPTIONS=halt_on_error=1 ./build/k2b-linux-parent/test_input_queue_tsan
```

本机日志在忽略的构建目录 `build/k2b-linux-parent/`：

- `offline-queue-20260924.log`（普通/NDEBUG/ASan+UBSan/交叉编译），SHA256
  `e5c09a3bef334b69bf046c6629ae99d0e818dede83ec081e4eebcdc7bf1f425e`。
- `offline-queue-tsan-20260924.log`（TSan 运行），SHA256
  `8591459b588d5e1b7742daa6fc43ea406cac0606157b9944f1a1002355f01caf`。

同时重跑原有 frame/access_unit/disp 普通和 NDEBUG 测试，各为
59/272/396 项通过。没有访问开发板、修改内核或启用新的平台选项。

## CedarC 内存组件离线落地

`5a7a751` 已实现实际 ScMemOpsS/DMA 系统调用路径、显式 VE 会话、
首错诊断、隔离资源和 pin 所有权检查。54 场景的普通、NDEBUG、
ASan/UBSan 及单独 TSan 复跑通过，ARM64 libMemAdapter.so 已交叉构建但未加载。
ABI 正反例及缓存头文件拒绝检查也通过；命令、产物/日志哈希、
真实硬件未验证边界详见 [私有运行库记录](k2b-cedarc-runtime.md)。

这个组件尚未与 CedarC 解码循环和 Moonlight 回调连接。下一步是项目
私有运行库构建/加载与解码工作线程接入，之后仍需解决、验证真实显示
退役和持续串流。未把内存适配器单测视作 1080p60 完成。
该离线检查点生成时开发板不可达，板端最后同步位置为 `acfd079`。

## 热点恢复后的原生复核

用户恢复热点并确认 IP 未变后，再次连接 `kickpi@10.33.184.81`。
实测仍为 KICKPI K2B、Linux 5.4.125/aarch64、glibc 2.35、CMA 128 MiB，
编译工具为 GCC 11.4.0、CMake 3.22.1。启动 ID 为
`66ed4017-1fae-4b40-82ca-877d257b5175`。没有发现解码/显示探针进程，
`/dev/cedar_test_heap` 不存在；本次没有加载模块或操作 HDMI。

核对板端工作树干净、分支正确后，验证传输包哈希并用 --ff-only 同步
`acfd079` → `cc57482`。配套头文件只展开在项目的
`build/k2b-vendor-headers`，不替换系统头文件；cedar_ve.h 哈希与本地一致。
板端已有 CedarC 原始归档的 SHA256 也与固定归档一致。

在板端原生编译运行：frame 59 项、access_unit 272 项、disp_config
396 项、input_queue 与 cedar_memory 54 场景均通过；ABI 正例编译通过，
错误 TINA 宏与换位 ScMemOpsS 均按指定门禁拒绝。内存测试依然只包装
系统调用，不访问真实 Cedar/disp。这证明原生用户态测试通过，不能当作
真实 DMA 或解码/显示通过。原有 `build/baseline` 的 Moonlight 构建亦成功，
没有执行生成程序。

主机保存日志 `build/k2b-cross/native-resumed-tests-20260924.log`，SHA256：
`a2d810e28e0faaf92d79532bc67ef43f6f3827f19bb036fd26e2a574041d8351`。
接下来的私有运行库接入需继续原生编译/解析验证，不沿用旧探针内存库。

## 私有运行库构建已接入

`15d0de0` 引入独立入口 `tools/k2b-runtime` 和顶层默认 OFF 的
`BUILD_K2B_CEDARC_RUNTIME`。固定归档展开在构建目录，生成五个源码库与
四个原样核心 blob，使用本项目内存适配器；完整九库链接不允许缺失符号。
这只是构建开关，不注册 k2b 平台，尚未把解码输出接入显示。

主执行者复跑本地交叉构建、ELF/哈希/导出及错误输入/缓存拒绝测试通过。
发现交叉产物 libcdc_base 需要 GLIBC_2.38 后，没有上传这些库；转用板端
GCC 11 原生构建，九库引用版本最高为 GLIBC_2.34（板端 glibc 为 2.35）。
默认 OFF 的旧基线和 ON 的完整 Moonlight 构建均通过；系统链接器 --list
诊断确认九个依赖解析到本项目私有目录。这不等价于 VPU 初始化成功。

板端源码同步到 `15d0de0`；原生 Release 产物在
`/home/kickpi/projects/moonlight-embedded/build/k2b-runtime-native/runtime`。
没有系统安装，没有启动生成的程序、注册插件、加载 CMA 模块或操作显示。
命令、警告、日志哈希和加载边界见 [私有运行库记录](k2b-cedarc-runtime.md)。
下一步仍需精确的 0x804 兼容层、运行时加载/注册检查与解码线程接入，
随后才是动态显示持有/退役及实际 1080p60 验收。

## 精确兼容层接入与原生 ABI 验证

`2840c0b` / `f7e5e01` 已实现显式 opt-in、严格 K2B/5.4.125 身份匹配的
0x804 偏移查询兼容。ARM64 汇编保留原始参数寄存器，解决通用 C 变参
包装不适用于所有 ioctl 调用形式的问题。库保持私有且不自动激活。

分发逻辑 245 项测试在本地普通/NDEBUG/ASan+UBSan 和板端普通/NDEBUG
下均通过。板端管道测试确认实际 ioctl 提供者、双参数和指针调用、errno
以及管道 0x804 拒绝路径；没有预加载时确实失败，不是假阳性。
原生独立、默认 OFF、集成 ON 的构建及 ELF 隔离亦已复核。
代码快进同步到板端 `f7e5e01`，没有运行真实 VPU 或切换 HDMI。
命令、源码证据和日志哈希见 [兼容层记录](k2b-cedar54-compat.md)。

另从实际 `/proc/config.gz` 确认 composer/sync_file 已启用，但 FTRACE
未启用；没有把 composer 配置存在视作帧退役已解决。详情追加在
[disp 同步审计](k2b-disp-sync-audit.md) 的只读运行配置章节。
下一步仍为私有运行时加载/注册与解码线程接入，再进行受控的真实解码、
动态显示退役及 1080p60 串流验收；本轮没有降低目标。
