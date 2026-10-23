# 安装与使用

[English](PC-SETUP.md) | 简体中文

整合包包含两部分。基础部分是 VD-PicoEnhance 和 SteamVR 中的 PICO 手柄电量显示，始终安装。**PicoHand<->Knuckles** 是可选模块，在 Valve Index 指虎（Knuckles）与 PICO 光学手追之间自动切换，首次安装时只有你加 `-WithKnuckles` 才会装上，装上之后升级会一直保留它。

## 安装前的准备

基础部分需要：

- 已 root 且开启 Zygisk 的 PICO 4 或 PICO 4 Pro，以及官方 Android VD 1.34.22.0；
- 装有受支持的 VD Streamer 1.34.22.0 和 SteamVR 的 Windows PC；
- PC 上的 Android platform-tools（`adb`），并在头显上授权调试和 shell 的 root 请求。

PicoHand<->Knuckles 适用于 PICO 4 和 PICO 4 Pro，不依赖眼追或面捕硬件。使用时还需要 SteamVR 中的 Valve Index 指虎，以及处于同一个私有局域网（10.x、172.16–31.x 或 192.168.x 地址）且能互相访问的 PC 与头显。

两种机型的头显后端目前均要求完整固件指纹匹配已验证的 PICO OS 5.13.7 构建。匹配值为 `Pico/Phoenix/PICOA8110:10/5.13.7/smartcm.1761755159:user/dev-keys`，可用 `adb shell getprop ro.build.fingerprint` 查看；版本号相同但指纹不同的固件也会被拒绝。

## 一次安装

1. 下载并完整解压 Release 整合包。退出 VD 和 SteamVR，将头显通过 USB 或已授权的无线 ADB 连接到 PC。
2. 在运行 SteamVR 的同一 Windows 账户中打开普通 PowerShell，在解压目录运行：

   ```powershell
   # 仅基础部分
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1

   # 基础部分 + PicoHand<->Knuckles
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -WithKnuckles
   ```

   adb 不在 PATH 中时加 `-Adb 'C:\path\to\adb.exe'`；连接了多台设备时加 `-Serial <目标设备>`；Streamer 安装在非标准位置时加 `-DriverPath <完整驱动DLL路径>`。PC 写入阶段会弹出管理员确认，无需将整个终端提权。
3. 安装器会修补 PC 上的 VD 驱动，使其发布手柄电量，同时安装头显模块和缺失的手部网格，并重启一次头显。使用 `-NoReboot` 时由你自己重启，模块在下次开机时生效。

加了 `-WithKnuckles` 时，安装器还会部署 PicoHand<->Knuckles：PC 上独立的 SteamVR driver、头显上独立的 daemon，以及二者之间一次性的配对。头显的 Wi-Fi 地址和 PC 面向头显的地址会自动读取；如果识别到了错误的网卡，加上 `-HeadsetAddress <地址> -PcAddress <地址>` 手动指定。头显重启后 daemon 随之运行。该参数需要完整安装，与 `-HeadsetOnly` 或 `-Mode 1` 同时使用时，安装器会直接报错而不是跳过它。首次安装不加该参数时，这个模块完全不会安装，头显的手追设置也保持原样；已经安装的模块会被保留并升级。

## 日常使用

正常启动 VD 和 SteamVR 即可，电量显示不需要其他操作。

PicoHand<->Knuckles 随 SteamVR 启动。它的 driver 通过局域网连接头显上的 daemon，不需要 ADB，不需要 VD 的画面数据，PC 上没有额外的虚拟设备或常驻程序，VD 是否运行都不影响它。测试时先启动 SteamVR，再打开 Index 指虎。

在自动模式运行时（这是默认状态，没有单独的启动步骤），指虎接入 SteamVR 并稳定定位后，请对**每只已开启的指虎分别触摸并完全松开触摸板一次**，包括暂时放在一旁不用的另一只。仍在线但还没有上报过触摸状态的指虎，模块无法判断它已经松开，这会阻止切回手追。

- **拿起：** 在移动指虎的同时触摸它的触摸板。触摸和有效运动必须来自同一只指虎。随后模块关闭系统手追（包括系统手势输入），手不再出现，使用指虎期间系统菜单和重定位手势也不会触发。VD 自己的**屏蔽系统手势**开关是另一项独立设置，保持你原来的选择。
- **放下：** 所有在线的指虎都松开触摸板，并保持静止 1 秒。系统手追恢复，光学手重新出现。只用一只指虎时同样适用。指虎仍拿在手上时也是如此：拇指离开触摸板且指虎保持静止，就会切回手追。需要留在指虎模式时，让拇指继续触碰触摸板。
- **没有指虎：** 模块待机，只向头显查询状态，不写手追偏好，也不改系统设置，PICO 手柄和光学手追照常工作。
- **信号丢失：** 指虎仍连接但触摸板状态未知或丢失定位，不算松开。所有指虎都消失时，模块恢复原状态并回到待机。

控制停止时（例如 Stop 或关闭 SteamVR），头显会恢复到原来的手追状态。PC 突然消失时，头显上的 3 秒控制租约到期，同样会触发这次恢复。

已在 PICO 4 Pro／PICO OS 5.13.7 上验证：左右指虎双向切换；指虎模式下不弹系统菜单、不重定位；放下指虎后手追与 PICO 菜单恢复。

### PC 如何找到头显

配对之后，driver 通过一条简短的 UDP 请求寻找头显：既发往保存的地址，也发往 PC 上处于活动状态的私网以太网和 Wi-Fi 接口的组播地址 239.255.255.250。头显应答后，PC 向应答来源的地址建立 TCP 控制连接。发现和控制都使用 29744 端口。面捕 Bridge（9030 端口）不参与，也不需要运行。

因此，同一局域网内任何一台设备换了地址，都不必重新配对。头显每秒检查一次自己的 Wi-Fi 地址；地址变化时，先恢复原来的手追状态，再在新地址上监听。网络失败后，自动模式会先恢复原状态，停顿约两秒后重试，重新查找并建立新的会话。SteamVR 从待机返回后，driver 可以重新发现头显并建立新会话。已经保存的 Stop 或停用选择会保持停止；头显报告的错误、协议错误以及未完成的恢复不会自动重试。

网络需要放行 UDP 组播，并允许两台设备之间使用 TCP 和 UDP 29744；开启客户端隔离的访客网络可能会阻断。没有头显应答时，PC 会尝试保存的地址，因此仍使用旧固定地址版的头显也能连上。

## 停用、重新启用与状态

在解压目录运行；停用和启用需要连接头显：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Disable
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Enable
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Status
```

Disable 和 Enable 只作用于 PicoHand<->Knuckles。Disable 恢复头显的手追、停止切换，并在重启头显和 SteamVR 后保持该选择；Enable 重新开启。二者都不影响基础模块和电量显示。

Status 读取本机保存的配置，显示 PicoHand<->Knuckles 是否已安装、是否启用，以及 PC 电量补丁的状态。它不连接头显和 SteamVR，显示的是已配置的内容，不能说明切换此刻是否正在工作。

## 升级、配对与卸载

- **升级：** 退出 VD 和 SteamVR，再运行新版整合包中的同一安装命令，从 1.0.8 升级同样如此。PC 补丁从原备份重建，头显模块升级并重启一次。已安装 PicoHand<->Knuckles 时，不必再加 `-WithKnuckles`，它会随之升级并保留启用或停用选择。
- **刷新配对：** 保存的地址已经过期、发现又无法弥补时（网络阻断组播，或其中一端仍是旧固定地址版），退出 SteamVR、连接头显，运行：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Configure
  ```

  需要手动指定地址时加 `-HeadsetAddress <地址> -PcAddress <地址>`。Configure 会重新配对两端，并保留启用或停用选择。只是 ADB 地址变化时，在所运行的命令上指定 `-Adb <路径> -Serial <新端点>` 即可。
- **只卸载 PicoHand<->Knuckles：** 退出 SteamVR、连接头显，运行：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action UninstallKnuckles
  ```

  这会恢复头显的手追，注销 SteamVR driver，并标记移除头显模块，头显下次重启时生效。VD-PicoEnhance 和电量显示不受影响。要重新安装，带 `-WithKnuckles` 再运行安装器。
- **完整卸载：** 退出 VD 和 SteamVR、连接头显，运行：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Uninstall
  ```

  已安装 PicoHand<->Knuckles 时，先将它停止、恢复手追并注销；然后恢复 PC 原驱动、标记移除头显模块并重启。`-NoReboot` 可将重启留给自己，移除在下次启动时生效。原驱动备份和配对信息保留。

## 安装器改动了什么

PC 修补只修改 Streamer 的 `OpenVRDriver/bin/win64/driver_VirtualDesktop.dll`，旁边保存精确原件和安装记录。补丁只承载电量显示，PicoHand<->Knuckles 是另一个独立的 driver。未知版本、外部修改或 SteamVR 运行中会被拒绝，Streamer 自行更新后也不会被旧备份覆盖。修补后的驱动在你的 PC 上生成，未签名，不需要关闭 Windows 安全策略；官方 Android APK 保持原样。

PicoHand<->Knuckles 会添加自己的 SteamVR driver 并向 SteamVR 注册。Valve 的 Lighthouse 驱动、指虎固件、绑定和校准保持原样。配对密钥只保存给你的 Windows 用户和 SYSTEM。

整合包不含任何 Virtual Desktop DLL（无论原件还是修补后的），修补是在你 PC 上已有的那一份上进行的。
