# EVB10 移植笔记（中文）

按时间顺序记录这块板子从 UEFI 到锁屏的过程，以及踩过的坑。结论都来自真板观测（旧仓库布局下），
没有在新的仓库布局里重跑；`porting.toml` 里的状态因此是 `implemented`，不是 `device-verified`。

## 里程碑

| 阶段 | 结果 |
| --- | --- |
| UEFI → XNU | edk2-rk3588 固件接管，DarwinBoot 把源码编的 4K XNU 跳起来，串口出内核日志 |
| 根卷 | eMMC 驱动读 GPT，发布 `userdata` 里的 OS / SystemOS 窗口，APFS 根卷只读挂载 |
| 用户态 | `launchd` 起来，约 110 个进程；原版 SpringBoard / backboardd 不再崩溃循环 |
| SMP | 八个核都在跑，核间中断走 `ICC_SGI1R_EL1` |
| 显示 | VOP2 的 Esmart3 窗口出现在 DSI 面板上，随后换成内核自己的 1072×1904 帧缓冲 |
| 数据卷 | `userdata` 最后 2049 MiB 做持久 Data 卷：首启格式化，重启直接挂载，写入 0 次失败 |
| 输入 | I2C5 上的 Goodix 触摸，轮询 10 ms；一个 HID 设备接到原版 IOHIDEventDriver |
| GPU | Mali-G610 → `Mali` 驱动 → PanVK → metal2vulkan → 原版 Metal 前端，锁屏壁纸和时钟整屏合成 |

## 坑清单（症状 → 原因 → 改法）

1. **内联汇编里的 `;` 是注释。** 目标是 Mach-O，走 Darwin 汇编器：`"dsb ishst; msr icc_sgi1r_el1, %0; isb"`
   只会汇编出 `dsb ishst`。曾经的「GIC 吞掉 ICC_SGI1R」「A55 的 CNTFRQ 是 0」「各核计数器偏移不同」都是这个笔误造成的假结论。
   多条指令用 `"\n\t"` 分隔；怀疑「寄存器写了没反应」时先用 `llvm-objdump --disassemble-symbols` 看指令在不在。
2. **次级核从 EL2 起来。** PSCI `CPU_ON` 拉起的核在 EL2，要在进 XNU 复位向量前降到 EL1（`HCR_EL2.RW` 必须为 1，
   否则 `eret` 后第一条 AArch64 指令就未定义）。
3. **UART 是 1.5 Mbaud。** 固件已经配好，驱动不要再动除数。内核控制台超过约 255 个字符的行会丢尾巴。
4. **面板下半部分闪烁。** 固件留给 HDMI0 的 Esmart2 和面板用的 Esmart3 同时取数，两路都欠载（`POST_BUF_EMPTY`）；
   `evb10-vop-es2=1` 关掉 Esmart2 后零欠载。Esmart3 还要在寄存器里划给 VP2（写回 U-Boot 的图层混合策略）才生效。
5. **Data 卷太小。** 256 MiB 会 `ENOSPC` 卡住 SpringBoard；512 MiB 能到锁屏；现在是 2048 MiB。
6. **vnode jetsam。** 没有 `/defaults/kern.maxvnodes` 时 xsmall 配置把它压到 1024，守护进程被一个个杀掉。OS profile 现在带 66560。
7. **帧大小别刚好 1080×1920。** backboardd 会按另一台设备的 1242×2208 渲染并要求缩放，整帧复制的 swap 校验会拒掉。用 1072×1904。
8. **触摸的坐标系。** 控制器说「configured 800×480」，实际 X 9..696、Y 10..1270，网格是 720×1280。
9. **PanVK 的黑矩形。** 写掩码为 0 的颜色附件被当作「写了」，锁屏时钟位置出现黑块；Mesa 补丁在 MaliMetal 仓库。
10. **撕裂。** 零拷贝扫描下，翻页前要等这一帧的 GPU 作业做完。
11. **补丁上下文。** 设备补丁不能改前面补丁的上下文；`zzz-boot-progress` 引用 `sheng-platform-power` 的上下文，
    格式 2 的系列里要先把它和 `native-smp-secondary` 放进 SoC 系列。

## 还欠的

- 滚动：加上帧率实验的启动参数（零拷贝扫描 + GPU 栅栏、GPU 750 MHz、设备的 YCbCr 滤波）后，稳态约 50 帧/秒（最好的一次手势 58），
  逐项数字和原因在 MaliMetal 仓库 `docs/notes.zh-CN.md` 的「帧率」一节。**`board.toml` 的默认参数没有带这些**，默认仍是拷贝式扫描、GPU 198 MHz。
  还差的是手势开头那几个重帧（渲染服务器的编码线程多出几毫秒）。GPU / CPU 频率固定，CPU 频率未管理。
- GPU 完成走轮询，没有中断；一个进程最多八个 group。
- 网络、音频、电源管理不在当前范围。
