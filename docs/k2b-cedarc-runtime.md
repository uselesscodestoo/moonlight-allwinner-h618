# K2B CedarC 私有运行库接入记录

本页记录 2026-09-24 离线核对，不是生产运行库已经接好的声明。
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
本次没有把这些构建步骤接入 Moonlight 顶层 CMake，也未开放 k2b 平台。

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

## 生产接入前仍需处理

探针 `cedar_mem.c` 的 checked/未知指针路径会 abort，close 会尝试释放
残留分配。生产路径必须把失败传递、显示持有和停止顺序纳入统一状态；
不能在 DE 仍可能读取时借用探针 close 的清理逻辑释放缓冲区。

保留 ENGINE_REQ → DMA 导入 → 显式 UNMAP → ENGINE_REL 顺序，并提供
带分配大小/偏移的借用 fd 描述。网络输入要在回调返回前取得自有副本，
再经有界队列送给受控的 CedarC 线程；不让 Moonlight 链节点悬空。
已实现的 access_unit 模块只解决这一复制边界，不是完整队列或解码器。

输入队列现已另行完成离线验证，记录见开发日志；尚未接入真实解码器。
[内存会话审计](k2b-cedar-memory-lifecycle.md) 进一步明确了 void 回调的
首错传播、显式会话、失败隔离和 CedarC 初始化非事务性回滚的限制。
不能将用户态适配器自己的引用归零等同于全部厂商内核状态已经干净。

显示持有/退役依然按 [disp 同步审计](k2b-disp-sync-audit.md) 的证据门槛
推进；不能用结构 ABI 编译通过或码流复制单测替代实际 1080p60 验收。
