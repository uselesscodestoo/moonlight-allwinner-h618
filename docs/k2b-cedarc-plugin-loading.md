# 固定 CedarC 插件加载路径核查

2026-09-24，只读检查本地固定归档的源码与 ELF，没有执行核心库。
适用范围仅 `243f2cbe84a817344d2502f4dd3d66b81338282f` 的
`library/aarch64-none-linux-gnu`，四个文件哈希见
[固定清单](k2b-cedarc-blobs.sha256)。不是动态加载已经验证的声明。

## 不能直接沿用探针的扫描入口

配套 `include/vdecoder.h:299` 声明 `void AddVDPlugin(void)`。
本版实现位于预编译 `libvideoengine.so`，不是当前可编译的 vdecoder.c。
其 ELF 符号地址为 `0x42d0`，大小 1360 字节。反汇编显示：

- `0x4350..0x4374` 用 PID 拼接 `/proc/%d/maps` 并打开文件。
- `.rodata` 中搜索串是 `libvdecoder.so` / `libcdc_vdecoder.so`；
  `0x439c..0x43b8` 对映射文本做 strstr。
- `0x4424..0x446c` 从该映射行提取目录并调用 scandir。
- `0x4560..0x4594` 按文件名子串挑选插件，`0x4530` 调用
  AddVDPluginSingle。该过程不是只允许 H.264 的白名单。

`cedarx_test/cedar_decode.c` 调用了 AddVDPlugin；探针的私有目录中只有
所需插件限制了实际扫描候选，但生产路径不应依赖目录碰巧干净。
成功返回也不能表示所有期望插件已成功注册：接口本身为 void。

## 单插件入口及成功判据的局限

`libvideoengine.so` 导出 AddVDPluginSingle (`0x4140`，396 字节)，
但本次使用的公共 vdecoder.h 没有该函数声明。观察到的流程是
`dlopen(path, 2)` → `dlsym(handle, "CedarPluginVDInit")` → 调用插件。
错误路径按日志级别打印日志并返回；成功路径尾调用插件，无可依赖的统一状态返回。
函数没有向调用者交出加载句柄，也没有在已检查函数内 dlclose。
不应凭它导出了符号就猜一个 int 原型来判断成功。

配套 `readMe.txt` 给出按需加载示例，
`openmax/libstagefrighthw/AwOMXPlugin.cpp:35` 明确定义
`typedef void VDPluginFun(void)`；调用方显式 dlopen/dlsym 后调用该初始化函数。
这支持按绝对路径加载
单个插件的方向，但示例本身没有提供完整的错误回滚/句柄所有权协议。

固定 `libawh264.so` 的 CedarPluginVDInit 位于 `0xee60`：
它向 VDecoderRegister 传入 format=`0x115`（配套 vbasetype.h 的 H264）、
CreateH264Decoder 函数指针及 bIsSoft=`0`。`0xee80` 调用注册，随后根据
结果打印日志，不向调用者提供可靠的注册结果。函数指针来源由
`0x2ef30` 的 R_AARCH64_GLOB_DAT → CreateH264Decoder 重定位核对。
这些证据支持这个插件注册的是硬件 H.264 creator，不能代替解码验证。

## 注册与句柄生命周期

配套 `vdecoder/include/videoengine.h:106` 声明有返回值的
VDecoderRegister(format, desc, creator, bIsSoft)。固定二进制
`0x22d0..0x25fc` 可见进程全局注册表；相同 format/desc 重复注册返回 -1，
成功保存 creator 指针。CreateSpecificDecoder 的 `0x1da4` 读取该指针，
`0x1dcc` 尾调用它。已检查动态符号表没有对应公开 unregister API。
因此后续加载包装不能在销毁一个 VideoDecoder 后就卸载插件：注册表中的
creator 指针仍可能被新解码器使用。重连不能重复盲调初始化入口。

该注册函数的 `0x23cc` 调用 cdc_malloc 后，`0x23e4` 直接通过返回指针
写 creator；配套 CdcMalloc.c 的 cdc_malloc 可以返回 NULL。
因此不能宣称这个 blob 的注册分配失败一定能安全报告给上层。
这是固定依赖的已知失败边界，不在这里修改二进制或伪造成功。

## 后续生产加载的约束

1. 只使用固定私有目录中的 H.264 插件，不调用扫描式 AddVDPlugin。
2. 在任何设备初始化之前，校验目标系统、所有库来源、真实动态依赖
   解析位置以及所需符号。单个绝对路径 dlopen 不证明嵌套依赖来自同目录。
3. 加载句柄与注册状态按进程生命周期管理；初始化串行化，正常重连复用，
   不以卸载/再加载来清理未知注册状态。
4. void 初始化返回不作注册成功证明。后续包装需明确注册成功观测策略；
   可以进一步核对并使用已声明的 VDecoderRegister 与固定 creator，但
   这里尚未实现或验证该替代调用路径。
5. 先完成真实库闭包构建和运行时解析自检，再进入内存 begin/解码器创建。
   完整链接成功只证明链接阶段的符号解析，不能替代上述运行时检查。

复核命令示例（不执行目标代码）：

```sh
blob=/path/to/libcedarc-tina/library/aarch64-none-linux-gnu
aarch64-linux-gnu-objdump -d --disassemble=AddVDPlugin "$blob/libvideoengine.so"
aarch64-linux-gnu-objdump -d --disassemble=AddVDPluginSingle "$blob/libvideoengine.so"
aarch64-linux-gnu-objdump -d --disassemble=VDecoderRegister "$blob/libvideoengine.so"
aarch64-linux-gnu-objdump -d --disassemble=CedarPluginVDInit "$blob/libawh264.so"
aarch64-linux-gnu-readelf -p .rodata "$blob/libvideoengine.so"
aarch64-linux-gnu-readelf -rW "$blob/libawh264.so"
```
