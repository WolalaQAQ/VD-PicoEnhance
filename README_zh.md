# VD-PicoEnhance

[English](README.md) | 简体中文

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: PICO 4 / PICO 4 Pro](https://img.shields.io/badge/platform-PICO%204%20%2F%20PICO%204%20Pro-lightgrey)

让 **PICO 4 和 PICO 4 Pro** 上**未经修改的官方** Virtual Desktop（`VirtualDesktop.Android`）用上 **PICO 光学手追**和 **Quest 式手部透视**，并在支持的硬件上提供**眼动手势指针修正**。它还能在 SteamVR 中**显示 PICO 手柄电量**，并可选地在 **Valve Index 指虎与手追之间自动切换**。

VD-PicoEnhance 是一个 Magisk/Zygisk 模块，在运行时提供这些功能，官方 Virtual Desktop APK 保持原样。完整模式还会向 PICO Shell 加载手势接收端，屏蔽功能由 VD 内的开关开启；在支持的固件上，启动助手运行于 `system_server`，让 VD 可以在手势模式下启动。

> 实验性，需要 root。已在 Virtual Desktop 1.34.22.0 上测试；升级 VD 后可能需要更新模块。

## 更新

各版本的功能与升级说明见[更新日志](CHANGELOG_zh.md)。本页对应 1.0.9；安装旧版本时，请阅读该版本标签下的文档。

## 功能

| 功能 | 做法 | 开关（`hand_gesture.txt`） |
|---|---|---|
| 手追扩展 | 让 Virtual Desktop 看到 `XR_EXT_hand_tracking`，并把该扩展注入应用的托管扩展列表 | 始终开启 |
| `XR_FB_hand_tracking_aim` | 层内合成捏合、菜单手势与指向射线；runtime 自身 aim 有效时优先使用 | `aim_*`、`pinch_*`、`menu_*` |
| `XR_FB_hand_tracking_mesh` | 用 `hand_mesh_fb.bin` 提供手部网格（模块内置，也可用你自己头显的资源生成） | 文件存在即启用 |
| PICO 原生手柄／手追切换 | 放下 PICO 手柄切到手，拿起手柄切回 | — |
| PICO手柄电量 | 通过VD原有字段传输，在本机修补的PC驱动中发布到SteamVR | 默认开启 |
| PicoHand<->Knuckles（可选） | 在Valve Index指虎（Knuckles）与光学手追之间自动切换：拿起指虎时关闭系统手追，放下后恢复 | 安装时加`-WithKnuckles` |
| 手势模式启动 VD | 在已支持固件上向系统声明 VD 的手追能力 | 完整模式自动启用 |
| 手部透视（Virtual Desktop 内） | 每只手一个独立的 projected 层；未绘制时恢复背景透视 | `pt_split`、`pt_bg_fix` |
| 手部透视（SteamVR） | 在 Virtual Desktop 串流画面的 swapchain 上原地改 alpha，只在手的位置开孔；不拷贝、不加层 | `pt_hole`、`pt_follow_settings` |
| 冻结门控 | runtime 关节数据长时间不更新时，把这只手报告为不活跃，缓解手势"亮一下就消失" | `hj_freeze_ms`（默认 300 ms） |
| Eye gaze | 仅对界面指针做 One-Euro 平滑、跳变确认和头部姿态补偿；串流眼动透传，两项增强默认开启 | `gaze_filter`、`gaze_vd_fix` |

SteamVR 里开孔的条件与 Virtual Desktop 自身的 `PassthroughPortals` 逻辑一致：串流源为 VR、`VRPassthroughHands` 打开、当前输入是手等。不在 SteamVR 时，模块不改动任何层。

眼动增强需要运行时确认眼追支持；普通 PICO 4 没有眼追硬件，也可使用手追与手部透视。整合版已在 PICO 4 Pro 上测试，贡献者也在 [PR #2](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/2#issuecomment-6263664529) 确认普通 PICO 4 工作正常。

## PicoHand<->Knuckles（可选）

如果你同时使用 Valve Index 指虎和 PICO 光学手追，这个模块会替你在两者之间切换：拿起指虎时系统手追关闭，放下后手追恢复。PC 根据 SteamVR 中真实 Index 指虎的触摸板触摸和运动，判断指虎何时被拿起或放下；头显上独立的 root daemon 负责把结果应用到系统手追设置。两端通过局域网通信，所以切换不需要 ADB，不需要 Virtual Desktop 的画面数据，PC 上也没有额外的程序：PC 端只是一个随 SteamVR 启停的小型 SteamVR driver。

下方统一安装器默认安装其余全部内容；首次安装时只有加 `-WithKnuckles` 才安装这个模块，装上之后升级会保留并更新它。日常只需正常启动 VD 和 SteamVR。拿起与放下的判定、网络条件、升级和卸载见[安装与使用指南](docs/PC-SETUP_zh.md)。

## 使用条件

| 组件 | 要求 |
|---|---|
| 头显 | **PICO 4 或 PICO 4 Pro**，arm64-v8a；眼动功能需要眼追硬件 |
| Root | Magisk，且**已开启 Zygisk**（Zygisk API v5，即 Magisk ≥ 27000） |
| Virtual Desktop | Android 版 `VirtualDesktop.Android` **1.34.22.0**，未修改 |
| PC（安装与使用） | Windows x64，已装 Virtual Desktop Streamer **1.34.22.0** 和 SteamVR；安装时需要 Android platform-tools（`adb`） |
| PC（仅构建用） | Windows + PowerShell、Android **NDK 26.1.10909125**、**CMake 3.22.1**、**platform 35**、**build-tools 35.0.0**、**JDK 17**、.NET SDK |
| PicoHand<->Knuckles（可选） | SteamVR中的Valve Index指虎；PICO 4或PICO 4 Pro，固件须满足[头显后端的匹配要求](docs/PC-SETUP_zh.md#安装前的准备)；PC与头显在同一私有局域网。root已是基础模块的要求 |

其他 Virtual Desktop 版本尚未验证。兼容层通过应用内存中的符号与 PICO runtime 的解析接口工作，但内部方法名与版本相关。

## 工作原理

Zygisk 模块匹配 Virtual Desktop 进程，把原生载荷 `libvdhs.so` 与托管 mod `VdHsMod.dll` stage 到应用私有目录，并在 **Virtual Desktop 自带的 OpenXR loader** 的导出函数上安装 inline hook。兼容层通过这些 hook 访问 PICO runtime：申请 PICO 手追扩展、合成 FB aim/mesh 数据、写入手部透视开孔。hook 作用于进程内存，磁盘上的 APK 和系统库保持原样。

组件划分、hook、能力检测和初始化过程见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。

## 目录结构

| 路径 | 内容 |
|---|---|
| `mod/native/` | VD 载荷 `libvdhs.so`、Shell 接收端 `libvdhs_shell.so`、追踪能力策略和共用已映射符号解析器；内置 `third_party/shadowhook-2.0.1/` |
| `mod/src/VdHsMod/` | 托管 mod（netstandard2.0，全程反射，不引用 Virtual Desktop 程序集） |
| `zygisk/` | Magisk/Zygisk 模块：注入器 `jni/`、`payload/`（`mode.txt`、`backend.txt`）、`build.ps1` |
| `coldstart/` | VD 专属系统包能力声明 helper 与构建脚本 |
| `pc/driver/` | PC电量代码载荷（在本机补入VD驱动）及其安装／恢复工具 |
| `knuckles/pc/` | PicoHand<->Knuckles的SteamVR driver |
| `knuckles/headset/` | PicoHand<->Knuckles的头显daemon与Magisk模块 |
| `knuckles/capture/` | 编入同一个自有SteamVR薄driver的触摸板采集代码 |
| `tools/mesh/` | 把头显自己的 PICO 手部 mesh 转成 `hand_mesh_fb.bin` |
| `assets/mesh/` | Release 附带的手部 mesh blob，含来源说明与免责声明 |
| `config/` | 含全部运行时开关的 `hand_gesture.txt` 示例 |
| `docs/` | [安装与使用](docs/PC-SETUP_zh.md)和架构说明 |

## 构建

把 `ANDROID_SDK_ROOT`（或 `ANDROID_HOME`）指向装有 NDK 26.1.10909125、CMake 3.22.1、platform 35 和 build-tools 35.0.0 的 Android SDK，也可以把 SDK 路径写进不纳入版本控制的 `tools/sdk-path.txt`。将 `JAVA_HOME` 指向 JDK 17，或把路径写入不纳入版本控制的 `tools/jdk-path.txt`。然后：

```powershell
# 原生载荷 + 托管 mod + Zygisk 注入器，stage 到 zygisk/payload/
pwsh -File zygisk/build.ps1

# 额外打包 zygisk/dist/vdhs_zygisk.zip（Magisk 安装包）
pwsh -File zygisk/build.ps1 -Pack
```

原生载荷要求零警告；默认的托管构建不还原任何 NuGet 包，可以离线编译。生成的 `zygisk/dist/vdhs_zygisk.zip` 可按下方[仅安装头显](#高级仅安装头显)步骤刷入 Magisk。

PC电量代码载荷用`pwsh -File pc/driver/build.ps1`构建，复用同一NDK工具链，首次下载固定版本的OpenVR头文件；可用`-OpenVRHeader <文件>`指定匹配的本地副本，构建不需要VD原件。PicoHand<->Knuckles用`pwsh -File knuckles/build.ps1`构建，产出`knuckles/dist/PicoHand-Knuckles.zip`（头显模块）和PC driver `driver_picomixedvr`。构建需要Python 3、Zig 0.13.0，以及与上文相同的NDK和JDK。`zig`不在PATH中时用`-Zig <路径>`指定；`-Python`、`-SdkRoot`、`-OpenVRRoot`可分别指定Python解释器、SDK和OpenVR SDK目录。默认会从固定的提交下载OpenVR 2.12.14的driver头文件并校验哈希。完成这些构建后，运行`pwsh -File tools/pack-release.ps1`，按明确的文件清单生成完整整合包，其中不含任何Virtual Desktop DLL。

## 安装

### 一次完成安装（推荐）

从 [Releases](https://github.com/WolalaQAQ/VD-PicoEnhance/releases/latest) 下载整合包 `VD-PicoEnhance-v<版本号>.zip`，在 PC 上解压。安装脚本、模块 ZIP 和手部网格会在同一目录中。先退出VD和SteamVR，连接已授权调试与root的头显，在该目录打开 PowerShell，用运行SteamVR的同一Windows账户执行：

```powershell
# 基础：VD-PicoEnhance 与 PICO 手柄电量
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1

# 基础 + 可选的 PicoHand<->Knuckles
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -WithKnuckles
```

基础安装会为电量显示修补PC上的VD驱动，安装头显模块和缺失的网格，并重启一次头显。只有PC文件写入时才会请求Windows管理员确认。已有网格保留，只有显式传入`-MeshPath`才替换。

`-WithKnuckles`额外安装PicoHand<->Knuckles：PC上独立的SteamVR driver、头显上独立的daemon，以及二者之间一次性的局域网配对，地址自动检测，也可用`-HeadsetAddress`和`-PcAddress`指定。首次安装不加该参数则完全不安装。它需要完整安装，与`-HeadsetOnly`或`-Mode 1`同用会被直接拒绝。升级时，已安装的模块保持安装状态，随整合包一起升级，并保留你的启用或停用选择，不必再次传入该参数。

安装完成后正常启动VD和SteamVR即可。电量自动显示，PicoHand<->Knuckles随SteamVR运行。ADB只在安装、配对和持久启停管理时由安装器使用。

所有内容都从解压后的整合包安装，脚本不下载文件。ADB可从PATH、SDK环境变量或`-Adb`指定，非标准Streamer位置用`-DriverPath`指定完整驱动DLL路径。

其他参数：`-Mode 1` 只做注入对照测试，`-NoReboot` 不自动重启，`-ZipPath` / `-MeshPath` 指定其他位置的本地文件，`-Serial` 在多台设备时指定目标。要安装其他版本，使用对应 Release 的整合包即可。

### 高级：仅安装头显

仅验证头显模块时可给统一安装器加`-HeadsetOnly`；它不改PC。也可在Magisk里选择**模块 → 从本地安装**，选择`vdhs_zygisk.zip`并重启。单独刷模块不包含PC电量显示和PicoHand<->Knuckles。

1. 如使用手动方式，先解压Release整合包，选择其中的模块ZIP。
2. 启动 Virtual Desktop，模块会自动放置缺失的 `hand_mesh_fb.bin`，无需手动复制。

`payload/mode.txt` 默认为 `2`，即完整兼容层。需要只做注入对照测试时，以 root 把 `/data/adb/modules/vdhs_zygisk/payload/mode.txt` 改成 `1`，然后重启头显。这会一并卸载 Shell 接收端和启动助手，仅重启 VD 无法卸载它们。

升级时，使用新版整合包重新运行安装脚本，安装完整模块并重启头显。

> 在已支持的 PICO 固件上，完整模式会向系统声明 VD 支持手追，让它可以在系统手势模式下启动。详见下方启动支持说明，无需修改 APK。

## 配置

默认使用无需配置文件。需要调整功能时，可将 [`config/hand_gesture.example.txt`](config/hand_gesture.example.txt) 中的相应条目写入设备上的 `/data/data/VirtualDesktop.Android/vdhs/hand_gesture.txt`。每个键都是可选的，缺省时沿用内置默认值；示例列出了可配置的键与建议值。

SteamVR 手部透视开孔默认开启（`pt_hole=1`），并跟随 VD 的“VR 手部透视”开关。界面眼动平滑和头部补偿默认开启；已有配置中的显式设置优先于默认值。保存配置后重启 VD 生效。

### 系统手势模式下启动 VD

在 PICO 4 Pro／PICO OS 5.13.7 上，完整模式默认在系统内存中的包元数据里为 VD 补充整数 `handtracking=1`，让系统原有的启动检查识别 VD 支持手追。官方 APK、签名、其他应用和全局手追设置均不变。helper 仅匹配已验证的 Android 10 固件指纹，其他固件保持原状。

安装后重启一次。以 root 执行 `sh /data/adb/modules/vdhs_zygisk/coldstart.sh status` 与 `sh /data/adb/modules/vdhs_zygisk/coldstart.sh query`。状态应显示 `applied` 或 `native`、值为 `int1`、结果为 `ok`；独立查询应显示 `metadata-query int1 ok`。确认后再用系统手势启动 VD。VD 更新后，启动助手会重新检查包对象。

排查问题时，可用 `coldstart.sh disable` 恢复本 helper 补入的声明并保存停用选择，`enable` 重新启用；升级保留已有显式选择。烟雾测试模式 `1` 不加载此 helper，切换模式或禁用整个模块后需重启。它与输入页的手势屏蔽开关是两项功能：这里只解决系统允许启动 VD，不屏蔽系统手势，也不负责指虎与手追切换。

### 系统手势屏蔽（实验性，默认关闭）

在 VD 的 **输入 → 屏蔽系统手势** 中控制；英文系统显示 **Suppress system gestures**。这是唯一的手势屏蔽开关，完整模式下始终显示，默认关闭并记住你的选择。切换通常在一到两秒内生效，无需重启 VD 或头显。

状态自动保存到 VD 私有目录的 `vdhs/system_gestures.txt`，无需手动编辑文件，已有的界面选择会保留。旧的模块级 `payload/system_gestures.txt` 不再使用；未保存或保存值无效时不屏蔽。

完整模式下，后台每秒检查一次已保存的界面状态，仅在值变化时更新 Shell 属性。接收开关状态的载荷保持加载，让用户随时可以开启；加载载荷本身不会启用屏蔽。

开启后，独立 Shell 载荷尝试让系统对 VD 使用与 PICO 自家串流应用相同的手势禁用判定，阻止 VD 聚焦时的重定位／Dock 手势；其他包沿用原逻辑。不向 OpenXR runtime 服务注入载荷，不修改全局手追开关。

取消勾选即恢复系统手势，开关仍保留。首次安装此修订后需重启一次头显以加载 Shell 接收端；禁用整个 Magisk 模块或切到烟雾测试模式 `1` 后，也需重启才能卸载接收端。

### 中文界面

汉化自动跟随头显系统语言：中文时使用简体中文词典，其他语言保持 VD 原界面，无需手动开启。模块在 VD 启动时读取系统语言，切换系统语言后重启 VD 生效。

汉化覆盖控制面板和 SteamVR 性能面板。电脑／游戏／文件名称、路径、网络名称、账号、时间、实时数值、设备名称和单位保留原文；“断开连接”“本地文件”等固定文字单独翻译。

需要自定义翻译时，以 root 编辑 `/data/adb/modules/vdhs_zygisk/payload/ui_zh.txt`，然后重启 VD。每行格式为 `英文<TAB>中文`，字面量 `\n` 表示换行。删除该模块词典后重启 VD，可关闭汉化。

### 手部网格

`XR_FB_hand_tracking_mesh` 需要 `hand_mesh_fb.bin`。它要和载荷放在一起，即应用私有目录：

```text
/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin
```

没有这个文件时，模块不向 VD 启用手部网格功能。这个目录在模块运行过一次后已经存在。

**使用内置网格（推荐）**。通常无需手动复制，已有网格会保留。要替换网格，可通过安装脚本的 `-MeshPath` 参数指定文件，或在 PC 上执行：

```sh
adb push hand_mesh_fb.bin /data/local/tmp/
adb shell
```

在设备 shell 中输入 `su` 获取 root，然后执行：

```sh
set -e
d=/data/data/VirtualDesktop.Android
owner=$(stat -c %u:%g "$d")
mkdir -p "$d/vdhs"
chown "$owner" "$d/vdhs"
chmod 0700 "$d/vdhs"
cp /data/local/tmp/hand_mesh_fb.bin "$d/vdhs/hand_mesh_fb.bin"
chown "$owner" "$d/vdhs/hand_mesh_fb.bin"
chmod 0644 "$d/vdhs/hand_mesh_fb.bin"
restorecon -RFD "$d/vdhs"
exit
exit
```

改完重启 Virtual Desktop。也可以直接在头显上用带 root 的文件管理器复制到上面的路径。

**自行生成**（需要 numpy）：

```sh
python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
# 再把 out/hand_mesh_fb.bin 按上面的方式复制进去
```

来源与免责声明见 [`assets/mesh/README.md`](assets/mesh/README.md)。

## 已知问题

- **SteamVR 里用捏合（扳机）切换主手不生效。** 手张开时扳机值约为 0.27、从不为 0，而 Virtual Desktop 只在扳机值等于 1.0 时才发出数字 click。候选修法是在托管层给扳机加死区并重映射，尚未测试。
- **Eye gaze** 转头时指针仍会轻微移动。
- runtime（或 SteamVR）偶尔会让手部关节长时间冻结，手势"亮一下就消失"，重启 SteamVR 可以恢复。
- 启动支持被停用或固件不在其支持范围时，仍可能显示 PICO 的“不支持手势”启动提示，见[系统手势模式下启动 VD](#系统手势模式下启动-vd)。
- Virtual Desktop 更新可能改动内部命名，导致兼容层失效，需要等适配。

## 待做

- [ ] **USB 有线串流**：Virtual Desktop 1.34.22 支持通过 USB NCM 有线串流，但 PICO 侧目前没有把这条链路开放给应用，所以应用里的 USB 模式默认不可用。计划是先让头显起来一个 USB 网络接口、再让兼容层用上它，在真机上验证链路和发现之后再决定是否做成正式功能。

## 卸载与回滚

- 只卸载PicoHand<->Knuckles：退出SteamVR、连接头显，运行`powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action UninstallKnuckles`。它恢复头显手追、注销driver并标记移除其头显模块，VD-PicoEnhance和电量显示不受影响。只想暂停而不移除时用`-Action Disable`，`-Action Enable`重新开启。用`-Action Configure`刷新配对的方法见[安装与使用](docs/PC-SETUP_zh.md)。
- 完整卸载：退出VD和SteamVR、连接头显，运行`powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Uninstall`。已安装PicoHand<->Knuckles时先将它停止、恢复并注销；然后恢复PC原驱动、标记移除头显模块并重启。原驱动备份保留。
- 注入对照：以 root 把 `/data/adb/modules/vdhs_zygisk/payload/mode.txt` 改成 `1`，然后重启头显。改回 `2` 时也需重启，让所有组件使用同一模式。
- 在 Magisk 里禁用或移除模块，然后重启。需要手动禁用时，以 root 执行：

  ```sh
  touch /data/adb/modules/vdhs_zygisk/disable
  ```

- 禁用或移除模块后，可删除 `/data/data/VirtualDesktop.Android/vdhs/` 清理暂存文件。这也会清除保存的开关、`hand_gesture.txt` 和自定义网格，请先备份需要保留的文件。

## 隐私与安全

- 模块需要 **root**，并向 Virtual Desktop 注入代码。请只在自己的设备上，安装你自行构建或信任的版本。
- 注入器将载荷放入目标应用的私有目录，不改 APK、清单、签名或包名。
- 完整模式会加载 Shell 接收端，即使屏蔽开关处于关闭状态；在支持的固件上，还会向 `system_server` 加载仅处理 VD 元数据的启动助手。这两个组件都不向 OpenXR runtime 服务注入，也不修改全局手追设置。
- 安装PicoHand<->Knuckles后，头显上会运行一个root daemon，它修改系统手追设置，并在放下、停止或连接中断时恢复原值。PC用配对密钥在局域网中找到这个daemon，任何一端换地址后也能继续找到。发现和控制都以明文发送该密钥，请在可信的家庭网络中使用，不要将TCP／UDP 29744映射到公网。PC凭据文件仅当前Windows用户和SYSTEM可读。日常运行不使用ADB，也不向VD添加hook。
- 本仓库不打包任何 Virtual Desktop、Xenko、Mono 或 PICO 的二进制或源码，唯一的例外是 Release 整合包内的手部 mesh blob，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
- 提交 Issue 时请给出头显型号、PICO OS、Virtual Desktop 版本和脱敏后的日志片段，不要上传设备标识或签名密钥。

## 免责声明

这是非官方的实验性社区项目，不包含或绕过Virtual Desktop、PICO及其授权。Android APK保持原样；PC安装器为电量显示修改本机VD驱动并保存原备份。root注入和PC修补可能影响应用使用，请遵守所用软件的条款。"Virtual Desktop" 与 "PICO" 归各自所有方所有，此处仅用于描述互操作性。

## 致谢

- 感谢 [misaka18848](https://github.com/misaka18848) 为普通 PICO 4 所做的兼容工作（[PR #1](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/1)）、运行时手势屏蔽开关与中文汉化（[PR #2](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/2)），以及对整合版本进行的 PICO 4 真机测试。
- [ShadowHook](https://github.com/bytedance/android-inline-hook)：inline hook 运行时。
- John "topjohnwu" Wu：Magisk / Zygisk 模块 API。
- `hand_mesh_fb.bin` 基于 PICO 系统手部 mesh，用 `tools/mesh/` 的离线工具转换。

## 许可证

本项目采用 [MIT](LICENSE) 许可证。第三方组件保留各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
