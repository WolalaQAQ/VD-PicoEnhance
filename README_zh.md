# VD-PicoEnhance

[English](README.md) | 简体中文

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: PICO 4 Pro](https://img.shields.io/badge/platform-PICO%204%20Pro-lightgrey)

让 **PICO 4 Pro** 上**未经修改的官方** Virtual Desktop（`VirtualDesktop.Android`）用上
**PICO 光学手追**、**Quest 式手部透视**和**眼动手势指针修正**。

VD-PicoEnhance 是一个 Magisk/Zygisk 模块，在运行时向 Virtual Desktop 进程注入一层很小的
OpenXR 兼容层。**不修改、不重签、不改名、不重打包** Virtual Desktop 的 APK——只是让应用用上
厂商自带 loader 从不去申请的 PICO 功能。

> ⚠️ **实验性、需要 root、与版本相关。** 模块会 hook 一个闭源商业应用，且 Virtual Desktop
> 更新后可能失效。安装前请阅读[使用条件](#使用条件)、[已知问题](#已知问题)与[免责声明](#免责声明)。
> 这是独立的社区项目，与 Virtual Desktop、PICO 官方无关。

## 功能

| 功能 | 做法 | 开关（`hand_gesture.txt`） |
|---|---|---|
| 手追扩展 | 让 Virtual Desktop 看到 `XR_EXT_hand_tracking`，并把扩展注入应用的托管扩展列表 | 始终开启 |
| `XR_FB_hand_tracking_aim` | 层内合成捏合/菜单手势和指向射线；runtime 自己的 aim 有效时优先用它 | `aim_*`、`pinch_*`、`menu_*` |
| `XR_FB_hand_tracking_mesh` | 用 `hand_mesh_fb.bin` 提供手部网格（Release 附带，也可用你自己头显的资源生成） | 文件存在就启用 |
| 手/手柄热切换 | 放下手柄切到手，拿起手柄切回 | — |
| 手部透视（Virtual Desktop 内） | 每只手单独一个 projected 层；暂停时恢复背景透视 | `pt_split`、`pt_bg_fix` |
| 手部透视（SteamVR） | 在 Virtual Desktop 串流画面的 swapchain 上原地改 alpha，只在手的位置开孔；不拷贝、不加层 | `pt_hole`、`pt_follow_settings` |
| 冻结门控 | runtime 关节数据长时间不更新时，把这只手报告为不活跃（PICO 手势"亮一下就消失"） | `hj_freeze_ms` |
| Eye gaze | One-Euro 平滑、跳变确认，并补偿 Virtual Desktop 在 PICO 上漏乘的头部姿态 | `gaze_filter`、`gaze_vd_fix` |

SteamVR 里开孔的条件和 Virtual Desktop 自己的 `PassthroughPortals` 逻辑一致（串流源是 VR、
`VRPassthroughHands` 打开、当前输入是手等）；不在 SteamVR 时不改任何层。

## 使用条件

| 组件 | 要求 |
|---|---|
| 头显 | **PICO 4 Pro**，arm64-v8a |
| Root | Magisk 且**已开启 Zygisk**（Zygisk API v5，即 Magisk ≥ 27000） |
| Virtual Desktop | Android 版 `VirtualDesktop.Android` **1.34.22.0**，未修改 |
| PC（仅构建用） | Windows + PowerShell，Android **NDK 26.1.10909125**、**CMake 3.22.1**、.NET SDK |

> 其他 Virtual Desktop 版本未验证。兼容层通过应用内存中的符号和 PICO runtime 解析接口，
> 但内部方法名与版本相关。

## 工作原理

Zygisk 模块匹配 Virtual Desktop 进程，把原生载荷 `libvdhs.so` 和托管 mod `VdHsMod.dll`
stage 到应用私有目录，并在 **Virtual Desktop 自带的 OpenXR loader** 的导出函数上安装 inline
hook。层通过这些 hook 访问 PICO runtime：申请 PICO 手追扩展、合成 FB aim/mesh 数据、写入手部
透视开孔。不动任何系统库或已安装文件。

完整设计、hook 列表、托管后端和健壮性约定见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。

## 目录结构

| 路径 | 内容 |
|---|---|
| `mod/native/` | 原生载荷 `libvdhs.so`：`vdhs_payload.c`（Mono 嵌入加载器）、`vdhs_layer.c`（OpenXR hook、gaze）、`vdhs_hand.c`（手追、aim/mesh）、`vdhs_pt.c`（SteamVR 透视开孔）、`vdhs_mark.c`；内置 `third_party/shadowhook-2.0.1/` |
| `mod/src/VdHsMod/` | 托管 mod（netstandard2.0，全程反射，不引用 Virtual Desktop 程序集） |
| `zygisk/` | Magisk/Zygisk 模块：注入器 `jni/`、`payload/`（`mode.txt`、`backend.txt`）、`build.ps1` |
| `tools/mesh/` | 把头显自己的 PICO 手部 mesh 转成 `hand_mesh_fb.bin` |
| `assets/mesh/` | Release 附带的手部 mesh blob，含来源说明与免责声明 |
| `config/` | 含全部运行时开关的 `hand_gesture.txt` 示例 |
| `docs/` | 架构说明 |

## 构建

把 `ANDROID_SDK_ROOT`（或 `ANDROID_HOME`）指向装有 NDK 26.1.10909125 与 CMake 3.22.1 的
Android SDK（也可以把路径写进不跟踪的 `tools/sdk-path.txt`），然后：

```powershell
# 原生载荷 + 托管 mod + Zygisk 注入器，stage 到 zygisk/payload/
pwsh -File zygisk/build.ps1

# 额外生成 dist/vdhs_zygisk.zip（Magisk 安装包）
pwsh -File zygisk/build.ps1 -Pack
```

原生载荷要求零警告；默认托管构建不还原任何包，可离线编译。

## 安装

1. 在 Magisk 里 **模块 → 从本地安装**，选择 `dist/vdhs_zygisk.zip`，然后重启。
2. 自带的 `payload/mode.txt` 是 `1`，只做注入冒烟测试。确认模块生效后，把
   `/data/adb/modules/vdhs_zygisk/payload/mode.txt` 改成 `2`，重启 Virtual Desktop 才启用完整层。
3. 之后只更新载荷时（不必整机重启），把 `libvdhs.so` 推到
   `/data/adb/modules/vdhs_zygisk/payload/`，重启 Virtual Desktop 即可。

> PICO 处于手势模式时会阻止 Virtual Desktop 冷启动。先拿起手柄，等
> `getprop sys.pxr.trackingservice.gesturemode` 变成 `0` 再启动 Virtual Desktop。

## 配置

把 [`config/hand_gesture.example.txt`](config/hand_gesture.example.txt) 拷到设备上的
`/data/data/VirtualDesktop.Android/vdhs/hand_gesture.txt`，按需调整。每个键都是可选的，
缺省就沿用内置默认值；当前设置和全部键名都在该文件里。

### 手部网格

手部网格（`XR_FB_hand_tracking_mesh`）由放在载荷旁边的 `hand_mesh_fb.bin` 提供；未提供时该扩展直接不可用。

- **现成 blob**：Release 附带 `hand_mesh_fb.bin`，由头显自己的 PICO 系统资源（`XRShell.apk`）经
  `tools/mesh/xrshell_mesh.py` 转换而来。推到
  `/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin` 即可。来源与免责声明见
  [`assets/mesh/README.md`](assets/mesh/README.md)。
- **自行生成**（需 numpy）：

  ```sh
  python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
  # 再把 out/hand_mesh_fb.bin 推到 <app_data>/vdhs/hand_mesh_fb.bin
  ```

`hand_mesh_fb.bin` 属于 PICO 系统资源，仅用于让互操作功能开箱可用，与 PICO 无隶属关系；如被要求会移除。

## 已知问题

- **SteamVR 里用捏合（扳机）切换主手不生效。** 手张开时扳机值约 0.27、从不为 0；Virtual
  Desktop 只在扳机值等于 1.0 时才发数字 click。候选修法是在托管层给扳机加死区和重映射，未测。
- **Eye gaze** 转头时指针还会轻微移动。
- runtime（或 SteamVR）偶尔会让手部关节长时间冻结，手势"亮一下就消失"，重启 SteamVR 可恢复。
- 头显内会显示 PICO 的"不支持手势"提示框，暂不处理。
- Virtual Desktop 更新可能改动内部命名，导致层失效，需等更新适配。

## 卸载与回滚

- 快速回滚：把 `payload/mode.txt` 改回 `1`，重启 Virtual Desktop，层会被完全跳过。
- 彻底删除：在 Magisk 里禁用/移除模块并重启：
  ```sh
  touch /data/adb/modules/vdhs_zygisk/disable   # 临时禁用
  rm -rf /data/adb/modules/vdhs_zygisk          # 彻底删除
  ```
- 可选清理 stage 出来的载荷：删除 `<app_data>/vdhs/`（即
  `/data/data/VirtualDesktop.Android/vdhs/`）。

## 隐私与安全

- 模块需要 **root**，并向 Virtual Desktop 注入代码。只在你自己的设备上、只安装你自行构建或信任的
  版本。
- 注入器只把文件 stage 到应用私有目录，不写别处；不改 APK、清单、签名或包名，也不动其他进程。
- 本仓库不打包任何 Virtual Desktop / PICO / Xenko / Mono 的二进制或源码，见
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
- 提交 Issue 时请给出头显型号、PICO OS、Virtual Desktop 版本和脱敏后的日志片段，不要上传设备标识
  或签名密钥。

## 免责声明

这是非官方的实验性社区项目。它不包含、不转发、也不绕过 Virtual Desktop、PICO 及其授权，不修改任何
已安装应用。对第三方应用使用 root 模块由你自行承担风险：可能弄坏 Virtual Desktop 或设备，也需要你
自行遵守所用软件的条款。"Virtual Desktop" 与 "PICO" 归各自所有方所有，此处仅用于描述互操作性。

## 致谢

- [ShadowHook](https://github.com/bytedance/android-inline-hook)：inline hook 运行时。
- John "topjohnwu" Wu：Magisk/Zygisk 模块 API。
- `hand_mesh_fb.bin` 基于 PICO 系统手部 mesh，用 `tools/mesh/` 的离线工具转换。

## 许可证

[MIT](LICENSE)。第三方组件保留各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
