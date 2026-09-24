# K2B 独立后端开发记录

## 当前状态

分支 `k2b-cedarc-disp`，基于 `h618-egl-download` 的 `4e870a2` 独立维护。
没有上游 PR 或远程推送。板端仓库为 `/home/kickpi/projects/moonlight-embedded`，
本地工作树为 `F:/temp/moonlight-embedded/.worktrees/k2b-cedarc-disp`。

已完成构建准备、严格 NV12 布局契约、厂商 disp 配置转换及压缩输入复制/队列，**尚未接入生产解码/显示后端，
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
