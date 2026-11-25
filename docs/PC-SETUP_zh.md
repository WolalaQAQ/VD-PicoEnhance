# PC 安装与日常使用

[English](PC-SETUP.md) | **简体中文**

Release 整合包包含两部分内容：

- **基础部分（默认安装）**：VD-PicoEnhance 头显模块 + SteamVR 中显示 PICO 手柄电量的本机 PC 驱动补丁。
- **PicoHand<->Knuckles（可选）**：在 Valve Index 指虎（Knuckles）与 PICO 光学手追之间自动切换。首次安装时只有加 `-WithKnuckles` 才会安装；装上后，后续升级会自动保留并更新。

## 安装前的准备

**基础部分需要：**
- 已 root 且开启 Zygisk 的 **PICO 4** 或 **PICO 4 Pro**，以及未修改的官方 Android Virtual Desktop **1.34.22.0** 或 **1.34.25.0**。
- 装有 Virtual Desktop Streamer **1.34.22.0** 或 **1.34.25.0** 与 SteamVR 的 Windows x64 PC。
- PC 上可用的 Android platform-tools（`adb`），并在头显上授权调试与 shell root 权限。

**PicoHand<->Knuckles 额外要求：**
- SteamVR 中已配对的 Valve Index 指虎。
- PC 与头显处于同一私有局域网（`10.x`、`172.16–31.x` 或 `192.168.x`）且可互通。PICO 4 与 PICO 4 Pro 均可使用，不依赖眼追或面捕硬件。
- 头显固件指纹须精确匹配已验证的 PICO OS 5.13.7 构建：

```sh
adb shell getprop ro.build.fingerprint
# 要求返回值：
# Pico/Phoenix/PICOA8110:10/5.13.7/smartcm.1761755159:user/dev-keys
```

> [!IMPORTANT]
> 头显后端会严格校验完整指纹。即使版本号同样是 `5.13.7`，指纹不一致也会被拒绝。

## 一次完成安装

1. 从 Release 下载并完整解压 `VD-PicoEnhance-v<版本号>.zip`。
2. 退出 **Virtual Desktop** 和 **SteamVR**，通过 USB 或已授权的无线 ADB 连接头显。
3. 在运行 SteamVR 的同一 Windows 账户中打开普通 PowerShell，进入解压目录并运行：

```powershell
# 仅基础部分（VD-PicoEnhance + 手柄电量）
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1

# 基础部分 + PicoHand<->Knuckles
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -WithKnuckles
```

写入 PC 文件时会弹出 Windows 管理员确认，同意即可，无需将整个终端提权。

安装器会就地修补 PC 上的 VD 驱动、刷入头显 Magisk 模块与缺失的手部网格，按需完成 PicoHand<->Knuckles 配对，并重启一次头显。

### 常用安装参数

| 参数 | 说明 |
|---|---|
| `-WithKnuckles` | 安装并配对 PicoHand<->Knuckles（需完整安装，不能与 `-HeadsetOnly` 或 `-Mode 1` 同用） |
| `-Adb <路径>` | `adb` 不在 `PATH` 或 `ANDROID_HOME` / `ANDROID_SDK_ROOT` 时手动指定 `adb.exe` |
| `-Serial <设备>` | 连接多台设备时指定目标序列号或无线 ADB 地址 |
| `-DriverPath <路径>` | Streamer 安装在非标准目录时指定 `driver_VirtualDesktop.dll` 完整路径 |
| `-HeadsetAddress <IP>` / `-PcAddress <IP>` | 指虎模块配对时手动指定头显和 PC 的局域网地址 |
| `-NoReboot` | 跳过自动重启，由你稍后手动重启（下次开机生效） |
| `-MeshPath <文件>` | 显式替换已有 `hand_mesh_fb.bin`（默认保留已有文件） |
| `-HeadsetOnly` | 仅安装头显模块，不改动 PC 文件 |
| `-Mode 1` | 设为注入对照测试模式（默认为完整模式 `2`） |

## 日常使用

正常启动 Virtual Desktop 和 SteamVR 即可，PICO 手柄电量会自动显示在 SteamVR 中。

**PicoHand<->Knuckles** 作为独立驱动（`driver_picomixedvr`）随 SteamVR 启停，不创建虚拟设备，无额外常驻程序，日常运行无需 ADB，也不依赖 Virtual Desktop 是否开启。建议先启动 SteamVR，再打开 Index 指虎。

> [!IMPORTANT]
> **指虎开机后的首次触碰**：每只已开启的 Index 指虎接入 SteamVR 并稳定定位后，请**分别轻触并完全松开它的触摸板一次**，包括你打算放着不用的那一只。在在线指虎首次上报触摸状态之前，模块无法确认它已松开，这会阻止系统切回手追。

### 自动切换规则

- **拿起**：移动指虎的同时触碰同一只指虎的触摸板。系统手追会立即关闭，双手不再出现，使用指虎期间也不会误触 PICO 系统菜单或重定位手势。VD 内的**屏蔽系统手势**开关独立工作，保持原有设置。
- **放下**：所有在线指虎松开触摸板并保持静止 **1 秒**。系统手追和 PICO 菜单手势自动恢复。（即使指虎仍握在手上，只要拇指离开触摸板且保持静止也会切回手追；需要停留在指虎模式时让拇指轻靠触摸板即可。）
- **无指虎在线**：模块保持待机，只向头显查询状态，不改写任何系统设置，PICO 手柄和手追照常工作。
- **丢失定位或断连**：处于未知触摸状态或丢失定位的指虎不算松开。所有指虎离线、退出 SteamVR 或 PC 突然断网时，头显都会恢复原本的手追状态；头显 daemon 设有 **3 秒控制租约**兜底。

### 局域网发现与 IP 变更

配对完成后，PC 驱动会同时向保存的地址和私网以太网 / Wi-Fi 网卡的组播地址 `239.255.255.250` 发送简短 UDP 发现请求，收到头显应答后再向来源地址建立 TCP 控制连接。发现与控制均使用 **29744 端口**（与 9030 端口的面捕 Bridge 无关）。

- **IP 变动免重新配对**：同一局域网内任意一端更换 IP 都不必重新配对。头显 daemon 每秒检查一次 `wlan0` 地址，地址变化时先恢复手追状态，再在新地址上重新监听。
- **断网重连与待机恢复**：遇到短暂网络波动或 SteamVR 待机返回后，自动模式会先恢复状态、等待约 2 秒后自动重连。手动保存的 `Disable` 状态，以及头显错误或协议错误不会盲目重试。
- **网络要求**：路由器需放行两台设备之间的 UDP 组播和 TCP/UDP `29744` 端口。开启客户端隔离的访客 Wi-Fi 可能拦截组播；无应答时驱动会回退尝试已保存的 IP。

## 常用管理命令

在解压目录执行以下命令。除 `Status` 外，其余操作均需通过 ADB 连接头显。

| 命令 | 作用 |
|---|---|
| `.\install-vd-picoenhance.ps1 -Action Status` | 读取本机配置，显示 PC 电量补丁状态以及 PicoHand<->Knuckles 是否已安装和启用（离线查询，不连头显或 SteamVR） |
| `.\install-vd-picoenhance.ps1 -Action Disable` | 暂停 PicoHand<->Knuckles、恢复头显手追并持久化保存（不影响基础模块和电量显示） |
| `.\install-vd-picoenhance.ps1 -Action Enable` | 重新启用 PicoHand<->Knuckles |
| `.\install-vd-picoenhance.ps1 -Action Configure` | 重新配对局域网凭据与地址（可加 `-HeadsetAddress` / `-PcAddress` 手动指定） |
| `.\install-vd-picoenhance.ps1 -Action UninstallKnuckles` | 恢复头显手追、注销 `driver_picomixedvr` 并标记移除头显指虎模块（下次重启生效） |
| `.\install-vd-picoenhance.ps1 -Action Uninstall` | 完整卸载：先移除指虎模块（如已安装），再恢复 PC 原版 VD 驱动、标记移除 `vdhs_zygisk` 并重启 |

在标准 Windows PowerShell 中执行时，在命令前加上 `powershell -NoProfile -ExecutionPolicy Bypass -File`，需要时补充 `-Adb` 或 `-Serial`。

### 升级方式

退出 Virtual Desktop 和 SteamVR，使用新版整合包重新运行 `install-vd-picoenhance.ps1`。PC 补丁会从已保存的原件备份干净重建，头显模块更新并重启一次。已安装 PicoHand<->Knuckles 时会自动升级，并保留原有的启停选择和配对信息。

## 故障排查

| 现象 | 排查方式 |
|---|---|
| 放下指虎后没有切回手追 | 对**每只**已开机的 Index 指虎轻触并完全松开触摸板一次，消除未知状态后再保持静止 1 秒。 |
| 指虎切换无法连接头显 | 确认 PC 与头显在同一私有局域网（未开启访客隔离），且 Windows 防火墙放行 TCP/UDP `29744`。如组播被拦截且 IP 已变更，运行 `-Action Configure` 刷新地址。 |
| 安装器拒绝修补 PC 驱动 | 彻底退出 SteamVR（确保 `vrserver.exe` 未运行），确认 Virtual Desktop Streamer 为 `1.34.22.0` 或 `1.34.25.0`。 |
| Streamer 更新后手柄电量消失 | Streamer 更新覆盖了 `driver_VirtualDesktop.dll`。退出 SteamVR，用适配新版 Streamer 的整合包重新运行安装器。 |

## 安装器改动了什么

- **PC 端 VD 驱动**：仅修补 Virtual Desktop Streamer 目录下的 `OpenVRDriver/bin/win64/driver_VirtualDesktop.dll` 以发布电量，并在同目录保留精确原件备份与安装记录。未知版本、外部改动或 SteamVR 运行中会被拒绝，后续 Streamer 更新也不会被旧备份覆盖。修补后的 DLL 在你本机生成、未签名，无需调整 Windows 安全策略。
- **PicoHand<->Knuckles**：注册独立的 SteamVR 驱动（`driver_picomixedvr`），并在头显安装独立 Magisk 模块（`pico_mixedvr_standalone`）。Valve Lighthouse 驱动、指虎固件、按键绑定与校准均保持不变。Windows 上的配对密钥仅限当前用户与 `SYSTEM` 读取。
- **Android APK**：始终保持原样。Release 整合包中不包含任何原版或修补后的 Virtual Desktop DLL。
