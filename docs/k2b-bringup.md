# K2B 独立后端开发记录

## 当前状态

分支 `k2b-cedarc-disp`，基于 `h618-egl-download` 的 `4e870a2` 独立维护。
没有上游 PR 或远程推送。板端仓库为 `/home/kickpi/projects/moonlight-embedded`，
本地工作树为 `F:/temp/moonlight-embedded/.worktrees/k2b-cedarc-disp`。

已完成构建准备、严格 NV12 布局契约和厂商 disp 配置转换，**尚未接入生产解码/显示后端，
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
