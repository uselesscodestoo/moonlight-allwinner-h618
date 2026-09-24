# K2B 5.4 Cedar 偏移查询兼容边界

2026-09-24。此文记录固定本地源码和 ELF 的核查，不宣称生产兼容库已通过 VPU 或显示测试。

## 为什么只处理 0x804

固定 CedarC 归档中的 `library/aarch64-none-linux-gnu/libVE.so`
SHA256 为 `3f27ba86b000a07b28cc5554f3658ff0bc3ac408189df91d48bfd109b2848528`。
反汇编 `veEnvGetIcVersion`：

- `0x2c40` 从上下文取 fd，`0x2c44` 设置第三参数为零。
- `0x2c48` 设置 request 为 `0x804`，`0x2c4c` 调用 `ioctl@plt`。
- `0x2c50` 保存返回值，`0x2c7c..0x2c84` 将它加到已映射的寄存器基址，再读取偏移 `240` 的寄存器。

这不是一个可以随意伪造成功的能力查询：返回值参与地址计算。
本地保存的同代 BSP `inspect/tina-cedar_ve_uapi.h` 从 `0x800` 顺序枚举到
`IOCTL_GET_VE_TOP_REG_OFFSET`，其值为 `0x804`；`inspect/tina-cedar_ve.c:718`
返回 `cedar_devp->ve_top_reg_offset`。源码还在第 1805 行读取设备树的同名属性。

上述两个本地证据文件位于独立 `cedarx_test` 探针目录，SHA256 分别为：

- UAPI：`4d069cd82c77f54da55ae9cfbadb14007a489a2660940eb1c06caa648679827d`
- 驱动：`5641988ea2a64eb36015dda73e551e626ba0185fada1bc128d08bbc7e0bfbc84`

K2B 厂商源码 `linux-5.4-h618/drivers/media/cedar-ve/cedar_ve.c` 没有这个查询分支；
该文件 SHA256 为 `c1e48a4722da0091d78c476e8091b664bc9e0fe23b3fe27717ff82ebeb3fbc27`。
`1669..1671` 的默认分支返回 `-1`。
`cedardev_mmap` 在 `1802` 从 `MACC_REGS_BASE` 起始处建立映射，
`1809..1810` 将该页帧映射到用户映射的开头，没有添加另一个 VE top 偏移。
因此本项目固定组合采用零偏移兼容，而不是伪造芯片 ID、解码结果或显示完成事件。
该结论不能推广到其他 BSP、其他芯片或同版本号但已修改的内核。

## 为什么不用探针的通用 C 变参入口

现有独立探针的 `ioctl(int fd, unsigned long request, ...)` 无条件读取一个
`unsigned long` 变参。真实调用者可能只传两个参数（例如 `FIOCLEX`），
也可能传指针；不能把一个已跑通的 Cedar 查询当成所有调用形式均透明的证明。

生产入口限定为 Linux AArch64 LP64：汇编直接保留 `x0/x1/x2`，尾调用固定三参数
隐藏 helper。helper 将第三寄存器视为原始位模式，不在 C 中猜变参是否存在。
其余 ioctl 直接通过 `syscall(SYS_ioctl, ...)` 转发，不经过递归的 `ioctl` 符号。
这保留 Linux 系统调用语义，不保留其他第三方 interposer 的额外行为；
未来私有启动入口必须排除不明预加载库。

## 精确匹配与失败行为

只有 request 精确为 `0x804`，且以下检查全部成功，才返回零：

- `CEDAR_K2B_KERNEL54_COMPAT` 精确等于 `1`。
- uname release 为 `5.4.125`，machine 为 `aarch64`。
- 调用者 fd 和 `/dev/cedar_dev` 均为字符设备，设备号一致。
- 完整设备树 model 为 `KICKPI K2B`，允许单个末尾 NUL；拒绝前缀匹配、附加字节、读取或关闭失败。

兼容层不打开 Cedar、不请求 ENGINE、不访问寄存器、不改变时钟、不缓存身份，
也不关闭调用者 fd。模型文件是唯一需要打开的文件。任何不匹配均转发真实系统调用，
兼容成功和转发前均恢复入口 errno，避免身份探测污染调用者状态。
其他 request 完全跳过环境和身份检查。

环境必须在线程启动前设定；该匹配路径不支持从 signal handler 调用。
身份检查是固定组合的误用防护，不是内核二进制完整性或对抗性安全边界。

## 验证分层

1. 本地单测只替换 getenv/uname/stat/fstat/open/read/close/syscall 边界，执行真实匹配逻辑。
2. 板端 ABI 测试只使用管道、fcntl 和 dladdr，确认实际 ioctl 提供者，再验证双参数、指针和错误转发。
3. 真实 Cedar 正例留待内存会话、加载注册和失败清理路径接通后验证。

第 1、2 层即使通过也不等于第 3 层，更不等于 1080p60 实际呈现通过。
不要用“打开 Cedar、查询、立即关闭”作为孤立硬件测试：厂商 close 路径存在共享 DMA
清理风险，必须遵守完整会话生命周期。独立探针保持不变，不在此阶段重新运行。

## 已验证检查点

`2840c0b` 完成固定三参数 C 分发及单测，还没有公共 ioctl 入口。
实现者先观察 ENOSYS 最小桩的返回值断言失败、两个基线用例通过，随后新增
边界测试再次观察到“未设置环境仍被错误截获”的失败，再补齐身份校验。
主执行者对最终提交分别强制重建运行普通、NDEBUG、ASan+UBSan 测试，
每次均为 245 项通过；独立规格审查与质量审查通过，无待修复项。

仓库根的普通测试命令：

```sh
make -B -f tests/k2b/Makefile test-compat CC=cc BUILD_DIR=build/k2b-compat-parent
# NDEBUG：另设 BUILD_DIR 并传 CPPFLAGS=-DNDEBUG。
# ASan+UBSan：另设 BUILD_DIR，CFLAGS 与 LDFLAGS 均加 -fsanitize=address,undefined；
# CFLAGS 使用 -std=c99 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer。
```

板端再次只读确认内核 5.4.125、CMA 128 MiB、启动 ID
`66ed4017-1fae-4b40-82ca-877d257b5175`。`/dev/cedar_test_heap` 和
`/etc/cedarc.conf` 仍不存在；本阶段没有打开 Cedar/disp 或加载模块。
ARM64 公共入口和管道 ABI 实测另行完成后才补充记录，不能以本节替代。
