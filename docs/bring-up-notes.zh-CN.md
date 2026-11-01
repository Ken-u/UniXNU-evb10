# EVB10 移植笔记（中文）

按时间顺序记录这块板子从 UEFI 到锁屏的过程，以及踩过的坑。下面的结论最初来自真板观测（旧仓库布局下）；
之后已在用这些仓库从零重编的系统上复现（2026-10-29、10-30，见 `docs/reproduce.md`），面板和触摸也由人在板子上看过、摸过，都正常。
`porting.toml` 里各区域的状态因此是 `device-verified`，每条设备证据对应 `evidence/` 里的串口摘录。

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
| Wi-Fi | AP6398S（BCM4359，SDIO）：内核下载固件、扫描、固件自带的 WPA2-PSK 入网，iPadOS 用它当 `en0`（DHCP、DNS、TCP 都是用户态自己发的），「设置」里 Wi-Fi 页的列表、输入密码入网、状态、忘记网络都在板子上由脚本化触摸 + 截图验过（5 GHz 不行）；默认关闭，`evb10-wifi=N` 打开，见 `docs/network.md` |

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
12. **渲染服务器的重放落在 A55 上。** 调度器把八个核当成同一种核，挑编号最小的空闲核；A55 排前面时，渲染服务器的编码线程
    （重放一个重命令缓冲：A76 上 4 ms，A55 上 11–14 ms）就在 A55 上。现在设备树把四个 A76 排前面（逻辑核 0–3），引导核（A55）排第 5；
    `cpumask=31` 只留这五个核，`mali-epri=50` 把编码线程提到优先级 50，`sched-slowcpus=240` 让界面线程在快核空闲或能被抢占时别去 A55。
    `mali-epri=50` 和 `sched-slowcpus` 要一起开才有用（2×2 实验：只开其中一个没有改善，只提优先级反而更差）：落在 A55 上的重放 8 %（只有 `cpumask=31`）→ 0.5–1.8 %。
    细节在 MaliMetal 仓库 `docs/notes.zh-CN.md`「帧率（二）」。
13. **A76 只有 1.78 GHz。** UEFI 的稳压器表因为 DSC 里用了不存在的宏而编成全零，A76 簇被升频却没有升压。已修（`uefi-evb10` 的 `fix(uefi)` 提交），
    实测 2.23 GHz（固件要的是 2.4 GHz，没有到，原因没查）；重放时间几乎没变（受访存延迟限制）。
14. **从核偶尔没起来。** 约 6 % 的轮次里 `evb10: smp running 0 conduit 1`：PSCI `CPU_ON` 发了，从核卡在开 MMU 那一步，整个 iPadOS 只在 CPU 0 上跑，
    时序和帧率全不可信。`run-kernel.sh` 检测到就自动重跑（`RETRY_SMP`，最多 3 次，丢弃的那轮存成 `TAG.smpN.*`）。**原因没查。**
15. **内核命令行最多 1000 字节。** `unixnu` 在 `payload` 步骤拒绝更长的（板子没有动）。`board.toml` 的默认是 884 字节；调试参数用
    `EXTRA_KERNEL_ARGS` 加，同名的替换默认值，总长要算。

16. **IOKit 一直不安静，用户态网络配置等了五分钟。** 症状：Wi-Fi 的 `en0` 早就起来了，iPadOS 的 DHCP 却在 330 秒才出现。
    原因：用户态的接口命名（configd）和 wifid 要等 IOKit 安静（`waitQuiet`）。原版 `AppleImage4` 匹配 `/defaults`，在 `start` 里
    无超时地 `waitForMatchingService(serviceMatching("IOAESAccelerator"))`，而没有任何东西提供 `IOAESAccelerator`，`defaults` 就一直忙。
    （读内核镜像里的代码确认的；一开始怀疑的「等电源根域」是假线索，那是一条无害的早期日志。）
    改法：打开 UniXNU 的软件 AES（`CONFIG_XNU_NATIVE_AES=y`、`board.toml` 里 `SoftwareAES` 组件和 `build-software-aes-bindings.py`），
    它只是 `IOAESAccelerator` 后面的软件 AES-CBC，不需要 SoC 加密引擎；之后启动日志约 26 秒 IOKit 就安静了。
    这个问题和 Wi-Fi 无关，只是 Wi-Fi 让它露了出来（`wifi-net: IOKit busy ...` 日志每秒列出忙的服务，安静后打印 `IOKit is quiet`）。
17. **入网要先设基础设施模式。** 固件默认是 ad-hoc（IBSS）：不调 `WLC_SET_INFRA 1` / `WLC_SET_AP 0` 直接 `bsscfg:join`，会得到「链路已连」事件
    和一个随机 BSSID，却永远没有四次握手。另外 `bus:txglom` 要设成 0。
18. **有了网络，Data 卷会被填满。** iPadOS 后台自己下载资源（`mobileassetd`、`nsurlsessiond`），2 GiB 的 Data 窗口在联网几分钟内写满
    （这两个守护进程收到 `ENOSPC`）；下一次启动空间又回来了。所以 Wi-Fi 保持「按参数打开」。
19. **SDPCM 发送是信用制。** 大流量下载时芯片的信用回得慢，发送队列会满：32 帧队列没有等待时，收 105 MB 丢了 12006 帧；
    现在队列 128 帧，`if_output` 最多等 40 ms，8 MB 的那一轮丢了 19 帧（重现 105 MB 没做）。
20. **「设置」里的 Wi-Fi 要靠 `wifid`，而 `wifid` 只在设备树里有 `wlan` 节点时才启动。** 症状：`en0` 好好的，用户态却没有任何进程来问它。
    原因：`com.apple.wifid.plist` 的 `_LimitLoadToDeviceTree = ["wlan", "marconi-wifi"]`，EVB10 的 ADT 里没有这个节点（`set2` 里没有 `wifid`）。
    处理：`dts/evb10.dts` 加一个只有 `unixnu,adt-name = "wlan"` 的节点（不写寄存器、不写 `interrupts`），`set3` 起 `wifid` 在 75 秒左右出现，
    开始用 Apple80211 ioctl 问 `en0`。顺带：当前网络（103）在 BSS 表里没有已入网的接入点时要从链路状态合成（否则 `ENOENT` 132 次）；
    383（接口属性表 plist）、470、471 这三个 set 每次状态变化都发，直接收下，否则守护进程反复重发。
21. **脚本化触摸 + 截图能代替人手验「设置」。** `evb10-touch-script=SECS`（`touch_script.inc`，git 忽略，由 `scripts/gen-wifi-touch-script.py` 按 `join.txt` 生成，
    密码只被翻成屏幕键盘的按键位置、不打印）无人值守地解锁（上滑两次，第一次总丢）、点「设置」、Wi-Fi、网络行、逐键输入密码、Join；`evb10-ps=12 evb10-shot-img=1
    evb10-shot-scale=2` 让串口日志每 28 秒带一张截图，`uefi-evb10/tools/xnu-probe/shot2png.py` 还原成 PNG。`evb10-wifi-join=2` 让开机不自动入网、扫描只列 join 文件里的网络
    （网络永远在第一行）。生成脚本带 `info` / `forget` / `rejoin` 参数还能点网络详情页、「忘记此网络」及确认、再入网一次。位置是 536×952 截图的像素（脚本里换算成千分比）。
    **坑：** 列表里的行会因为信号弱消失，点偏了会把密码打进别的输入框（一次打进了可见的「名称」框，截图里是明文）：截图只留在 `tmp/`，不要进提交；那种日志要把 `evb10: img` 行删掉。
    早先「点按打不开设置」是 `iconservicesagent` / `Preferences` 没有 Metal 设备（插件白名单和内核沙箱授权）加上 Mali 驱动对文件页的钉住，不是点按到不了（坑 22）。
22. **「设置」自己要能起来：`iconservicesagent` 和 `Preferences` 要有 Metal 设备，Mali 对文件页要拷贝不要钉住。** `iconservicesagent` 经 Core Image 画「设置」的图标，没有 Metal
    设备就退到 OpenGL ES（这台没有），在 `CI::GLContext::init` 跳到 0 崩掉（t19）：`NativeMaliMetalPlugin.mm` 的 `clientProcess()` 名单和 `package/Mali` 的沙箱授权都要点这两个名字。
    Mali 驱动里 `BO_CREATE_HOST` 遇到文件页（应用的映射资源，渲染服务和应用共享）要拷贝：钉住会占着文件对象的 activity 计数，而那个文件最后一个映射被解除时要等它归零，
    等的恰好是退役 GPU 工作的那个线程。症状是 `Preferences` 起来了、主线程卡在 `CASGetDisplays`（`evb10-bt=Preferences` + `dsc.py sym` 看到，t39–t41）。
    **更大的坑：子模块里提交了，必须马上在 `UniXNU-evb10` 里 `git add package/Mali`**，否则下一次构建（`unixnu`）会把子模块重置回索引里的旧提交，改动悄悄消失（t39–t41 就是这样白跑的）。
23. **「设置」里 Wi-Fi 的行要 MobileGestalt 说有 Wi-Fi。** 虚拟机模式（`vmm-present`）下它从 `/chosen` 的 `mac-address-wifi0` 回答 `wifi` 能力；没有这个属性就是全零地址、能力为假，
    行被隐藏。`WifiNet.cpp` 的 `publish_registry()` 在驱动拿到模块地址后写它。MobileGestalt 的答案缓存在 Data 卷（首次启动写），旧 Data 卷要删一次
    `com.apple.MobileGestalt.plist`（`UniXNU/docs/guide/mobilegestalt.md`）。没打开 `evb10-wifi=N` 的默认启动在这次改动后没看过。
24. **入网后行上的转圈要等包过滤器的 tap。** 勾号出现在 `IO80211InterfaceIPConfigurationBusy` 变假的时候；地址配置的 ARP 监视通过 `/dev/bpf` 读路由器的应答，自己实现的 ifnet 要在
    `ifnet_init_eparams.set_bpf_tap` 里给函数并对每个收发帧（含以太网头）调用它（`WifiNet.cpp` 的 `if_set_bpf_tap`）。没有 tap 时 busy 持续 64 秒（t36），有了 4.5–9.5 秒（t37、t42、t43）。
    固件还只放行登记过的组播，`allmulti=1`（`Evb10Wifi.cpp`）才收得到 IPv6 邻居发现。
25. **5 GHz 不行，锁到 2.4 GHz 也不是出路。** 固件会自己挑 5 GHz 的接入点：漫游过去之后 30 秒只多了 96 字节 TCP 载荷（t42）；忘记网络后再加入选到 5 GHz，关联无应答（事件 7 状态 5），「设置」弹「无法加入网络」（t43）。
    试过 `WLC_SET_BAND`=2（`WLC_UP` 之前）把无线锁在 2.4 GHz：扫描只剩 2412 MHz，但 t46 的两次加入都在 0.5 秒后以事件 0 状态 3（找不到网络）失败，而不锁的首次加入（t36、t37、t42、t43）都是 1.7–1.8 秒成功，所以这个开关已经删掉。
    没试的线索：CLM blob 没加载（文件是 2021 年的 1.39.1，固件内置的是 2023 年的 9.7.7）、nvram 里 `ccode=0 regrev=0`、发射功率；2.4 GHz 的扫描 RSSI 每次不同（t38 是 -44 dBm，t44–t46 是 -80 dBm）。

## 还欠的

- 滚动：`board.toml` 的默认参数现在带着帧率的全套（零拷贝扫描 + GPU 栅栏、设备的 YCbCr 滤波、GPU 750 MHz、编码线程避开 A55）。
  板上（`v1`，默认参数，演示手势）：手指按下期间手势 3–11 平均 52.6 帧/秒，最好 57.5；重命令缓冲从提交到完成 p50 14.9 ms、p90 18.9 ms，8.7 % 赶不上 19.3 ms 的期限。
  GPU 1 GHz 是选项（要 `GPU_RAIL_UV=850000` 的固件和 `mali-npll=1000`，写法在 `docs/reproduce.md`）：赶不上的降到 1.6–2.5 %，12 分钟稳定。
  逐项数字和原因在 MaliMetal 仓库 `docs/notes.zh-CN.md` 的「帧率」「帧率（二）」两节。离 60 帧还差：触摸到第一帧的延迟（右滑 150 ms 以上，推测是 Today View 的小组件）、
  手势开头的重帧、频率固定（GPU 不调频，CPU 频率未管理）。
- GPU 完成走轮询，没有中断；一个进程最多八个 group。
- Wi-Fi：数据通路是轮询的 SDIO（约 25 MHz，无 DMA、无中断），下载峰值约 34 Mbit/s；「设置」里的 Wi-Fi 页用脚本化触摸验过列表、输密码加入、状态、忘记网络（坑 20–24）。**5 GHz 不行**（见坑 25）；开放网络、WPA3、隐藏网络、重启后是否记得网络、没带 `evb10-wifi=N` 的默认启动没试。
- 音频：接到 iPadOS 了（`docs/audio.md`，`evb10-audio=4` + `evb10-audio-hal=1`，默认开）。内核侧：I2C7 上 ES8388 在 0x11 应答；I2S0 做主，12.288 MHz 的 MCLK 来自 GPLL，48 kHz；
  内核服务 `Evb10Audio`（`audiomxd` 打开它）+ 音频服务器插件（`hal/Evb10AudioPlugIn.c`，打包进 GPU addon，bindfs union 挂在 `/System/Library/Audio/Plug-Ins/HAL` 上，cdhash 进 owner trust cache）。
  板上（`evidence/audio-ipados-20261101.txt`，`aud1`–`aud3`）：音频服务器加载插件、读设备属性、设音量、起 IO（线程优先级 97，每 21 ms 提前 1531–2049 帧写 1024 帧）；
  脚本化触摸输密码的键盘点击声让发送 FIFO 在 48 kHz 下收到 1890782 帧（混音峰值 8748，音量 −21.6 dB 后进 FIFO 的最大样本 727），接收 FIFO 同速，无 panic。
  **没人听过**：耳机检测脚读 0（不知道有没有插东西，所以走了耳机功放，`evb10-audio-out=3` 两个都开）、爆音、音量曲线、录音质量都没人确认；只播过键盘点击声。
  **还有掉帧**：FIFO 靠一个轮询线程喂（32 字 = 667 µs），内核调度停顿超过 0.7 ms 就丢帧：39 秒里 4 次共 13 ms。教训：这个线程自己打印串口日志（0.7 ms 一行）一次停了 68 ms，日志已搬到别的线程。
  下一步：有人听一下（`evb10-audio-selftest=10` 不用 iPadOS 也能出一段三角波），然后上 DMA。视频编解码没动（iPadOS 走 `AppleAVDUserClient`，这个内核没有）。
- 电源管理不在当前范围。
