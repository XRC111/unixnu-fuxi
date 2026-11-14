# unixnu-fuxi：小米 13（fuxi）的 UniXNU 设备仓库

[![Build](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/build.yml/badge.svg)](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/build.yml)
[![Device documentation](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/porting-record.yml/badge.svg)](https://github.com/OddlyLabs/unixnu-fuxi/actions/workflows/porting-record.yml)

Device repository for the Xiaomi 13 (fuxi, SM8550), created from UniXNU's `examples/device` template. It builds with the
fork [OddlyLabs/UniXNU](https://github.com/OddlyLabs/UniXNU) (branch `fuxi`).

主文档：[fuxi_touch_panel_porting.md](fuxi_touch_panel_porting.md)，下一步计划：[fuxi-bringup-plan.md](fuxi-bringup-plan.md)。
各子系统的状态以 [porting.toml](porting.toml) 为准。

## 现状

| 子系统 | 状态 |
|---|---|
| 触控（ST FTM5） | 驱动和分阶段探针已写好。2026-11-02 第 3 阶段上机通过：上电、时钟、引脚和 12 MHz SPI 都正常，复位后收到就绪事件，读到芯片 ID `36 40`。第 4 阶段通过：轮询和中断两种模式都读到了完整的点击（按下、移动、抬起和坐标），两根手指也能分别跟踪，8 个核都在运行时也正常。触控电源必须由探针投 RPMh 票，不投时控制器不应答。开中断的启动共 8 次，有 1 次在内核启动几秒后自己重启，原因还没查清。HID 层（`ShengHidInput`）已接入并上机验证：事件驱动挂在 sheng 默认的组合设备上，每组触点都作为 HID 报告送进系统（一次 68 组，全部成功） |
| 固件和启动镜像 | CI 构建 DEBUG 固件和 `fastboot boot` 用的启动镜像，只上传加密版本。2026-11-02 上机：bootloader 接受镜像，固件、UniXNU 菜单和 DarwinBoot 正常，XNU 启动到 `bsd_init`。途中修了两处：缺 sheng 的 AMP 补丁（CPU 拓扑）；内核按树莓派的固定地址读写 panic 日志区（打开 `CONFIG_XNU_DT_PANIC_REGION`）。记录见 `evidence/2026-11-02-first-boot.md`。固件内存表已按 8 GB 和 12 GB 两台手机修正（uefi-fuxi `c5793bc2`） |
| 存储（UFS） | **2026-11-14 打通到真机**。接入 ShengUfs 的 `NativeUfs`：接管 UFS 控制器、过 SMMU、读 GPT、校验根 manifest，从 `unixnu` 分区读出两个 IPSW 窗口并逐字节核对（`evidence/2026-11-14-ufs-gpt.md`、`-ufs-root-windows.md`）。分区从 userdata 尾部切出 32 GiB，Android 侧数据实测完好（`-dual-system-partition.md`） |
| 存储根策略 | 用 `kits/VerifiedRoot` 给两个窗口签所有者策略。**策略是 header-only 的**：内核里只放 544 字节信封，每 4 KiB 块的 SHA-256 表放进根镜像的 `OS.vroot`、`SystemOS.vroot` 数据对象。原因是全量信封让 `boot.img` 涨到 203.6 MiB，而 fuxi 的 ABL 拒绝任何超过 `boot_a` 分区（192 MiB）的镜像；header-only 把它压回 138 MiB（`evidence/2026-11-14-header-only-policy.md`） |
| RAM 块设备 | 2026-11-03 接入 `RamBlock`：引导器交来的内存盘作为块设备发布，APFS 可以挂上去，用来从内存启动 IPSW 里的恢复盘。加 `rd=md0-ramblock` 后 `launchd`、`restored_external` 都跑起来了，这是这台手机上第一次跑起 iOS 的用户态 |
| CPU | 2026-11-02 上机：`sheng-smp=1 armcpu-spi-ipi=1` 时 7 个副核都经 PSCI 启动成功，调度器认到 8 个核；再加 `armcpu-self-ipi=1` 核间中断才通。两个小核（1、2 号）还没见到时钟中断。2026-11-03：去掉 `armcpu-spi-ipi=1` 时核间中断送不到（SGI 在这台机器上不通）；9 次多核启动里有 1 次副核全都没起来，原因未知 |
| 显示 | 参数已提取并在运行中的系统上核对过；只读探针读固件的 `FB_HANDOFF` 记录（见下文「显示线」）。完整实现还差 `NativeLegacyDisplay` 的 prelink 候选 |

### 存储线走到哪一步

`native-ufs-probe=11` 与 `native-ufs-root=1` 是两回事，需要分清：

- **probe 模式**（已多次上机）只做到「读到窗口并核对 SHA」，屏幕上的 `NATIVE_UFS root windows … manifest status=0`
  就是它的报告。这条路不碰签名策略。
- **root 模式**（2026-11-14 起）走 `configureVerification`：内核里的策略信封 → 从根镜像 `OS.vroot` 取哈希表 →
  `vr_open` 验签 → `signedVolumeUuid()` 返回真 → 发布根卷。这是 `bsd_init` 能从 UFS 挂根的前提。

真机已经确认的部分：`unixnu` 分区上的 manifest 是 version 2，四个条目全部正确

```
OS               off=0x100000     size=6077580800  blk=512   sha=e5634d04…
SystemOS         off=0x16a600000  size=3303014400  blk=512   sha=b4908cde…
OS.vroot         off=0x22f400000  size=  47481120  blk=4096  flags=3
SystemOS.vroot   off=0x232200000  size=  25804800  blk=4096  flags=3
```

`flags=3` 是 data object，`readPolicyTable` 正是按这个名字和大小去找的。

### 越过 Apple 密封检查

2026-11-14 第一次跑 root 模式，panic 屏上出现的是：

```
rootvp not authenticated after mounting @bsd_init.c:1107
```

**这一行本身就是进展**：它在 `vfs_mountroot()` 和 `VFS_ROOT()` 都成功之后才执行，所以 NativeUfs 已经发布了
根卷，内核也解析并挂载了它。之前所有运行都停在「读窗口」这一层，从没走到根卷发布。

失败点是下一句：

```c
int autherr = VNOP_IOCTL(rootvnode, FSIOC_KERNEL_ROOTAUTH, NULL, 0, vfs_context_kernel());
if (autherr) {
    panic("rootvp not authenticated after mounting");
}
```

`FSIOC_KERNEL_ROOTAUTH` 问的是 APFS：这个卷是不是 Apple Image4 密封过的。我们的卷由所有者自己的
VerifiedRoot 策略签名，所以 APFS 答「不是」。

**试过但走不通的做法**：`bsd_rooted_ramdisk()` 只比较 `rd` 参数的前三个字符，所以 `rd=md0-ramblock`
能跳过这段检查。但 `rd` 在 `IOKitBSDInit.cpp` 里还负责选根设备：`mdevlookup` 要求参数恰好三段
（`rdBootVar[3] == 0`），而 `md0-ramblock` 不满足，于是退到 `IOBSDNameMatching("md0-ramblock")`，
匹配不到任何设备，`waitForService` 会一直等下去。**用 `rd` 跳过检查会连挂载一起弄坏。**

**实际采用的做法**：`scripts/build-fuxi-root-auth.py` 把那个 `panic` 换成一行报告。这个改动只降低
「Apple 是否认可密封」这一关，不改变「数据是否可信」：NativeUfs 在发布每个 4 KiB 块之前已经按所有者
策略验过哈希，未经所有者签名的读取照样失败。

它是 **archive 步骤**而不是补丁：触控探针和显示探针都已经在改 `bsd_init.c`，第三个基于 hunk 的补丁
每次都要跟着重算行号（CI 上连挂两次，先是 `:943` 再是 `:957`）。`kernel.py` 先应用补丁系列、后跑
`[kernel] archives`，所以脚本看到的是补丁后的文件，直接匹配那条 panic 语句本身，与行号无关。它幂等，
语句位置变了就报错退出，不会瞎改。

### launchd 跑起来了

越过密封检查之后，2026-11-14 的启动到了这里：

```
initproc failed to start -- exit reason namespace 6 subcode 0x1
description: Library not loaded: /usr/lib/libSystem.B.dylib
 Referenced from: <4B525992-…> /sbin/launchd
 Reason: tried: '/usr/lib/libSystem.B.dylib' (no such file, no dyld cache)
```

**这是这台手机上第一次从 UFS 根启动用户态进程。** `launchd` 真的被 exec 了，卡在加载
`libSystem.B.dylib`。

问题在于那个库在 iOS/iPadOS 上**本来就不是文件**：它在 dyld 共享缓存里，而缓存又可能来自另一个卷。
iPadOS 17 的布局大致是：

```
OS 卷    → /System/Library/Caches/com.apple.dyld/
SystemOS → /System/Cryptexes/OS/…
```

我们只发布了 OS 和 SystemOS 两个窗口，`/System/Cryptexes/OS` 还没挂载。

### 下一个探针：先看，再决定挂什么

与其猜该挂哪个卷，不如让内核把看到的说出来。`package/FuxiRootCheck.cpp` 在 `bsd_init` 里、显示探针
之后、文件系统起来之前，用 `namei` 逐个查这些路径：

```
/usr/lib/libSystem.B.dylib          /System/Cryptexes
/usr/lib/dyld                       /System/Cryptexes/OS
/System/Library/Caches/com.apple.dyld   /System/Cryptexes/App
/System/Library/dyld                /sbin/launchd
/private/var                        /private/preboot
```

每个路径报告存在与否（目录会标 `(dir)`），最后给一行汇总。它只读、不改任何东西，由启动参数
`fuxi-root-check=1` 打开。有了这份清单，下一步该挂哪个卷就是从观测得出的，而不是试出来的。

## 显示线

面板和显示引擎的参数都已从原厂设备树提取，并在运行中的 Android 上核对过一遍
（`facts/display_engine_facts.json`）：

| 项 | 值 |
|---|---|
| 面板 | `qcom,mdss_dsi_m3_38_0c_0a_dsc_cmd`，屏身份码 `0bba0c4e` |
| 分辨率 | 1080×2400，DSI command mode，DSC 8 bit/component 与 8 bit/pixel，slice 540×40 ×2 |
| 帧率 | 60 / 90 / **120** Hz（运行中的系统实测 `1080x2400x{60,120,90}cmd`） |
| DSI 时钟 | 1.1 GHz |
| 背光 | `panel0-backlight`，max 4095（与 `qcom,mdss-dsi-bl-max-level` 一致） |
| 密度 | 440 dpi，413.755 × 413.849 |
| HDR | 类型 1-4，420 nits |
| MDP | `0xae00000+0x84000`，IRQ 83；ctl0 `0x16000`、mixer0 `0x45000`、intf1 `0x36000`、vig0 `0x5000`、dsc `0x81000`、cdm `0x7a200` |
| DSI | ctrl0 `0xae94000+0x1000`，phy0 `0xae95500`（common `0xae95000+0xa00`） |

### 固件怎么把帧缓冲交给内核

`PE_current_console` 描述的是控制台选的显示，不一定是真正扫描输出的那个。固件里负责编程面板的驱动
另外写了一条记录，并且在 base、width、height、pitch 全部就位之后才最后写上 magic
（uefi-fuxi `Silicon/Silicium/SiliciumPkg/Include/Library/FrameBufferHandoff.h`）：

```c
#define FB_HANDOFF_MAGIC   0x4F474246   /* "FBGO" */
#define FB_HANDOFF_OFFSET  0x40
typedef struct {
  UINT32 Magic, Width, Height, PitchBytes;
  UINT64 Base;
} FB_HANDOFF;                            /* 24 bytes */
```

记录落在 `Display Reserved` 区末尾：`Region.Address + Region.Length - 0x40`。这个区就是设备树里的
`cont_splash_region`，即 `0xb8000000 + 0x2b00000`，所以记录在 `0xbaafffc0`。两个数通过启动参数传进去
（`fuxi-display-base`、`fuxi-display-size`），探针不去走内存映射表。

`PE_parse_boot_argn` 原生接受 `0x` 十六进制（`getval` 看到 `0x` 前缀就把 radix 设成 16），所以这两个参数
按写法直接解析。

### 代码现状

`package/FuxiDisplayProbe.cpp` 是**只读**探针：

1. 读 `FB_HANDOFF`，报告宽高、pitch 和基址，并核对是否等于面板的 1080×2400 与 4 字节/像素；
2. 用 `PE_current_console` 取一次控制台描述符做交叉验证，两者几何不一致就报告；
3. `fuxi-display=2` 时再读固件留下的 DPU 管道状态（ctl0 layer0、intf1、vig0 fetch）。

它由 `patches/xnu/fuxi-display-probe.patch` 在 `bsd_init` 里调用（紧跟触控探针），默认不发声。

完整的显示契约（`NativeLegacyDisplay` 绑定到原始 MobileFramebuffer）还需要目标 OS 版本的 prelink 候选，
这个移植暂时没有，所以先做不依赖它的那一半。

### 寄存器语义的来源

和 UniXNU-sheng 一样，不靠猜：`facts/display_inputs.json` 把 Linux mainline 的 msm DPU 源码固定到具体
commit，逐个文件记下字节数和 SHA-256：

```
drivers/gpu/drm/msm/disp/dpu1/dpu_hw_mdss.h          8569 bytes
drivers/gpu/drm/msm/disp/dpu1/dpu_hw_ctl.c          23907
drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c         19535
drivers/gpu/drm/msm/disp/dpu1/dpu_hw_sspp.c         22891
drivers/gpu/drm/msm/disp/dpu1/dpu_hw_catalog.c      22008
drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_9_0_sm8550.h  10877
drivers/gpu/drm/msm/msm_mdss.c                      15472
```

其中 4 个文件与 sheng 记录的哈希一致，说明 SM8550 的显示块是同一个，偏移可以直接对照。已经交叉核对的一项：
`dpu_9_0_sm8550.h` 的 `max_linewidth = 5120`（0x1400）与设备树的 `qcom,sde-sspp-linewidth` 完全一致。

`facts/display_inputs.json` 里还写明了三条限制：探针还没上机跑过、XNU 显示契约缺 prelink 候选、
寄存器采样推不出刷新率和 VBL。

## 相关仓库

| 仓库 | 作用 |
|---|---|
| [OddlyLabs/UniXNU](https://github.com/OddlyLabs/UniXNU) | fork 自 UniXNU/UniXNU。分支 `fuxi`：组件目录加了 `FuxiFtsTouch`；`bootimg` 加了 fuxi 需要的两个选项（镜像头加载偏移、把负载放进内核镜像） |
| [OddlyLabs/uefi-fuxi](https://github.com/OddlyLabs/uefi-fuxi) | fork 自官方固件 UniXNU/uefi-sheng。分支 `fuxi`：移植了 matejkoo217/uefi-fuxi 的 9 个 fuxi 提交（保留原作者），这些提交里的结论都在真机上测量过 |
| [OddlyLabs/FuxiFtsTouch](https://github.com/OddlyLabs/FuxiFtsTouch) | 触控组件：FTM5 协议和触点跟踪，纯 C，带主机测试；本仓库用子模块引用它 |
| 本仓库 | 板级文件、探针、启动钩子补丁、硬件资料 |

UniXNU 自己提供启动菜单（UniXNUPkg）和 XNU 加载器（DarwinBoot），但它们是 UEFI 模块，要靠板级 UEFI 固件才能在手机上运行。fuxi 的固件选的是官方 uefi-sheng 加上经过真机验证的 fuxi 提交：官方部分可以直接合并上游更新，fuxi 部分来源清楚。

## 目录

| 路径 | 内容 |
|---|---|
| `board.toml`、`feeds.toml`、`configs/fuxi_defconfig` | 板子、组件、归档顺序、SoC 版本、内核配置；`[userdata]` 描述 `unixnu` 分区的窗口布局 |
| `dts/fuxi.dts` | 设备树（包含 UniXNU-sm8550-common 的 `sm8550.dtsi`） |
| `patches/xnu/` | `fuxi-touch-probe.patch`（`bsd_init` 调用触控探针）、`fuxi-display-probe.patch`（调用显示探针）、`fuxi-owner-root-auth.patch`（根卷由所有者策略密封时继续启动，见「存储线」） |
| `package/` | 在构建树里是 `kits/fuxi`。`FuxiTouchProbe.cpp`（探针）、`FuxiTouchBoard.h`、`FuxiPanelBoard.h`（板级常量，每项注明来源）；`FuxiRamBlockStore.cpp`（RAM 块的弱定义桩）；`TouchBoard.h`、`QcomRpmh.*`、`SmmuBypass.h`、`ShengPlatformPower.cpp`（AMFI 需要的 PMGR 桥）原样取自 UniXNU-sheng |
| `scripts/build-fuxi-touch.py` | 用 XNU 构建的编译记录编译探针和驱动，打包成 `libFuxiTouch.a` |
| `scripts/build-arm-driver.py` | 用同一份编译记录编译 catalog 里的 ARM 驱动（`NativeUfs` 走它）；对 NativeUfs 做三处设备侧覆盖，见文件开头 |
| `scripts/build-fuxi-display.py` | 用同一份编译记录编译显示探针，打包成 `libFuxiDisplay.a` |
| `scripts/build-fuxi-root-auth.py` | archive 步骤：把 `bsd_init` 里 ROOTAUTH 的 panic 换成一行报告（见「越过 Apple 密封检查」） |
| `scripts/build-fuxi-root-check.py`、`scripts/build-fuxi-root-check-call.py` | 编译根布局探针并在 `bsd_init` 里调用它 |
| `package/FuxiRootCheck.cpp` | 只读探针：报告挂根之后哪些关键路径存在（见「下一个探针」） |
| `package/FuxiDisplayProbe.cpp` | 只读显示探针：取固件交过来的扫描输出并报告几何，`fuxi-display=2` 时读 DPU 管道状态 |
| `scripts/build-fuxi-root-policy.py` | 给两个 IPSW 窗口签所有者策略，产出 544 字节信封和 `<窗口>.vroot` 哈希表；`--no-archive` 只做镜像侧产物 |
| `scripts/build-fuxi-platform-power.py`、`ci/check-pmgr-bridge.py` | 编译 PMGR 桥（改自 sheng 的构建脚本，改动见文件开头）；CI 在链接后核对它和 AMFI 的约定 |
| `scripts/build-fuxi-ram-block.py`、`ci/check-ram-block-bindings.py` | RAM 块设备及其绑定核对 |
| `scripts/pull-fuxi-gpu-firmware.sh`、`scripts/squash-mdt.py`、`scripts/data/fuxi-gpu-inputs.json` | 从手机提取 Adreno 740 的 ZAP（`.mdt` + 分片合并）、SQE 和 GMU 固件，并把字节数和 SHA-256 固定下来；固件本身在 `.cache/`，不入库 |
| `scripts/dual_sys-gpt-entry-growth.patch` | 把 fuxi 的 GPT 条目数 32 扩到 128（在已保留的 16 KiB 内） |
| `drivers/FuxiFtsTouch` | 子模块，指向 FuxiFtsTouch |
| `porting.toml` | 移植记录（UniXNU 格式） |
| `tests/board_test.c` | 板级常量测试 |
| `ci/boot-image-recipient.pem`、`ci/decrypt-boot-image.sh`、`ci/check-boot-image.py` | 启动镜像加密用的公钥、本地解密脚本，以及启动前核对镜像的脚本 |
| `facts/` | 从官方设备树导出的触控、面板、显示引擎、GPU 和保留内存事实，73 组面板 DSI 命令序列，以及显示寄存器语义所依据的 mainline 源码清单 |
| `evidence/` | 每一条上机结论，按日期，写清命令和看到的东西 |

本地另有 `dtb/`、`firmware/`、`power/`、`ref_src/` 四个目录，不进 Git（见 `.gitignore`）：官方 DTB 和触控固件是厂商文件；dmesg 里有使用痕迹；`ref_src/` 是 GPL 参考源码，只供查阅。

## 构建

需要 macOS 和 Xcode 26（UniXNU 也支持 Linux 工具链，见其文档）。三个仓库放在同一个目录下：

```bash
git clone --recurse-submodules https://github.com/OddlyLabs/unixnu-fuxi
```
```bash
git clone -b fuxi https://github.com/OddlyLabs/UniXNU
```
```bash
git clone -b fuxi https://github.com/OddlyLabs/uefi-fuxi
```

在 UniXNU 目录里依次运行：

```bash
./unixnu defconfig fuxi_defconfig -D ../unixnu-fuxi
```
```bash
./unixnu ipsw fetch OS "Cryptex1,SystemOS"      # 策略签名要读原始窗口
```
```bash
./unixnu build kernel
```
```bash
./unixnu build payload --uefi ../uefi-fuxi --unique-chip-id 0x0123456789abcdef
```
```bash
./unixnu build firmware --uefi ../uefi-fuxi --target DEBUG
```
```bash
./unixnu bootimg --uefi ../uefi-fuxi --target DEBUG --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb
```

`--unique-chip-id` 这里用的是固定的测试值。它只影响内核看到的板级身份，不影响启动；真正进用户态之前要换成手机自己的值。

固件这一步不涉及苹果的文件，也可以在 Linux 上单独跑，适合只改固件时快速验证：除了 clang、lld 和 llvm 工具，BaseTools 还要 `uuid-dev`。2026-11-01 在 Ubuntu 26.04（WSL，clang 21.1.8）上用上面的 `defconfig` 和 `build firmware` 两条命令构建通过，固件卷 0x300000 字节。

### 根镜像（`unixnu` 分区）

分区里的镜像由 `./unixnu build userdata` 生成，携带两个 IPSW 窗口和两个哈希表对象：

```bash
./unixnu build userdata --device "$PWD/fuxi" \
  --object "OS.vroot=build/verified-root-os/OS.vroot" \
  --object "SystemOS.vroot=build/verified-root-systemos/SystemOS.vroot"
```

两个 `.vroot` 来自 `scripts/build-fuxi-root-policy.py`。它们必须和内核里的信封出自同一次签名，否则密钥、卷 UUID
和哈希表会对不上。

## CI（GitHub Actions）

| 任务 | 运行环境 | 检查内容 |
|---|---|---|
| Host tests | Ubuntu | FuxiFtsTouch 测试（gcc/clang，ASan/UBSan）、板级常量测试、脚本语法 |
| Kernel-target compile | macOS 26 + Xcode | 驱动的 C 代码按 arm64 内核代码编译，确认不调用 libc |
| fuxi boot image | macOS 26 + Xcode | 编译内核（XNU 构建、归档、链接、prelink），确认触控探针在内核里、`bsd_init` 调用了它；然后构建 payload、固件和启动镜像，加密后上传 |
| userdata image | macOS 26 + Xcode | 组装构建树、签两个窗口、把 `.vroot` 作为数据对象写进根镜像，加密后上传 |
| Porting record | Ubuntu | 用 fork 里的 UniXNU 检查器验证 `porting.toml` |

启动镜像和根镜像里都有苹果的内容，所以只上传加密过的版本，私钥只在机主自己的电脑上；其余产物只上传文本日志。

`ci/build.yml` 把 `unixnu/out/ipsw` 缓存起来（键 `ipsw-21H461-os-systemos-v1`）：策略签名要哈希两个窗口共 8 GiB，
不缓存的话每次重建都要重新下载半小时。缓存只在构建成功后保存，坏下载不会变成后续信任的副本。

## 上机

只用 `fastboot boot` 临时启动，不写任何分区。步骤：

1. 下载 CI 的 `fuxi-boot-image-encrypted-*` 产物，用私钥解密：`bash ci/decrypt-boot-image.sh <私钥> <产物目录>`。
2. 核对镜像：`python3 ci/check-boot-image.py <产物目录>/boot.img <产物目录>/boot.json ../uefi-fuxi`。它按
   `board.toml` 和固件配置检查镜像头、加载偏移、固件卷、payload 里的文件、ADT 和启动参数，全部 `ok` 再继续。
3. 手机进入 fastboot，运行 `fastboot boot boot.img`。
4. 重启手机即回到原来的系统。

### 镜像大小上限（重要）

fuxi 的 ABL **不接受比 `boot_a` 分区更大的启动镜像**，即使 `fastboot boot` 只是把镜像投进内存。`boot_a` 是
201,326,592 字节（192 MiB），超出的镜像会被这样拒绝：

```
Booting   FAILED (remote: 'Failed to load/authenticate boot image: Load Error')
```

而且被判失败的镜像会把 slot 标成不可启动，之后每次重启都回到 bootloader：

```
slot-unbootable:a:yes
```

一条命令就能救回来，不用重刷：

```bash
fastboot set_active a
```

这也是根策略必须做成 header-only 的原因：全量信封让镜像到 203.6 MiB，正好越界。

### 探针镜像：定位 XNU 早期卡在哪

XNU 自己的控制台出来之前，能看到的只有屏幕顶部的色条：绿色是 DarwinBoot 交出控制权，蓝色是进入内核，品红是即将打开 MMU。再往后，就用会自动重启的"入口探针"来判断：探针放在某个位置，内核走到那里就等几秒然后重启手机。所以手机重启了，说明走到了那里；一直停在色条上，说明在那之前就卡住了。

做法遵循 UniXNU 调试指南（Entry probe reset 一节）。内核由 CI 编译，payload 和启动镜像在本地生成，不需要 Mac：

1. 解密产物：除了 `boot.img`，还会在旁边得到没有打过补丁的 `kernel.macho`。
2. 导出这次测过的固件卷，保证探针镜像里的固件和测过的完全相同：`python3 ci/check-boot-image.py boot.img boot.json ../uefi-fuxi --fd-out fd.bin`。
3. 在设备仓库的一份临时副本里，给 `board.toml` 的 `[payload]` 加上探针设置（不要提交），例如 `entry_probe_reset_delay = 6`（打开 MMU 之后重启），或者再加 `entry_probe_reset_after = "_arm_vm_init"`（`arm_init` 调用完它之后重启）。
4. 在 UniXNU 目录里依次运行（`-D` 指向那份临时副本）：

   ```bash
   ./unixnu build payload -D <临时副本> --uefi ../uefi-fuxi --kernel <kernel.macho> --unique-chip-id 0x0123456789abcdef
   ```
   ```bash
   ./unixnu bootimg -D <临时副本> --uefi ../uefi-fuxi --fd <fd.bin> --dtb ../uefi-fuxi/Platforms/Xiaomi/fuxiPkg/FdtBlob/sm8550-xiaomi-fuxi.dtb -o probe.img
   ```

   Linux 上需要 clang、lld、llvm 工具、`dtc` 和 `mtools`。
5. `fastboot boot probe.img`。设了延时的探针镜像不需要拍照：手机自己重启回系统，就说明走到了探针的位置。

### 大镜像写入分区

`unixnu` 分区有 32 GiB，`fastboot flash` 会在 Windows 上因为 8.8 GiB 的缓冲直接
`std::bad_alloc`，`adb shell` 又会因为 PTY 把二进制流写坏（`write EOF`）。可行的做法是用
`adb exec-in` 直接喂给 `dd`，不经过 PTY：

```bash
adb exec-in "dd of=/dev/block/by-name/unixnu bs=8M conv=fsync" < userdata-root.img
```

## 维护

- 上游更新：把 UniXNU/UniXNU 的 `main` 合并进 OddlyLabs/UniXNU 的 `fuxi`；把 UniXNU/uefi-sheng 的 `sheng` 合并进 OddlyLabs/uefi-fuxi 的 `fuxi`。
- 更新后改 `.github/workflows/build.yml` 里固定的两个提交（`UNIXNU_REF`、`UEFI_REF`），让 CI 重新跑一遍。
- 改动了哪个子系统，就同步更新 `porting.toml` 里对应区域的发现或证据，并运行记录检查。

## 本地测试

```bash
cd drivers/FuxiFtsTouch && for t in fts-protocol touch-core; do gcc -std=c11 -Wall -Wextra -Werror -I. Test/$t.c FtsProtocol.c FtsTouchCore.c -o /tmp/$t && /tmp/$t; done
```

## 许可证

BSD-2-Clause-Patent，见 [LICENSE](LICENSE)。
