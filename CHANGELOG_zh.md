# 更新日志

[English](CHANGELOG.md) | 简体中文

## 1.0.9 — 2026-10-18 — 屏蔽系统手势

- **VD 运行时不再触发系统手势**。重定位/打开 dock 这些系统手势是由系统 shell 执行的，不在应用内，所以 VD 里的层拦不住。逆向 shell（`com.picoxr.xrshell`）后找到了 PICO 自家串流应用的做法：shell 内部有一份硬编码的包名名单——`gDisableSystemGesturesPackageNames`，在静态初始化里构建，且只有一项 `com.picovr.picostreamassistant`——`InputManager::updateAppDisableSystemGestures(pkg, displayState, isSelf)` 每帧重新比对它并据此设置 `gApplicationDisableSystemGestures`。新增第三个 Zygisk 目标（`com.picoxr.xrshell`，载荷 `libvdhs_shell.so`）hook 该函数，对 `VirtualDesktop.Android` 给出同样的结果：VD 处于前台时 shell 不再执行手势，VD 一失焦即恢复。
- **runtime 侧配套载荷**：`libvdhs_rt.so` 注入 PICO 的独立进程 OpenXR runtime（`com.pico.xr.openxr_runtime`），在 VD 占用 MR 服务（`sys.pxr.mrserivce.user`）期间清掉 runtime 转发给各会话（包括 shell）的手势状态。它 hook `InputDeviceManager::service_status_callback`，并在其库加载后 hook runtime 自身的的手势事件发射函数（靠它打印的日志字符串定位），在事件投递前拦下。
- **共用符号解析器**：内存内 ELF 解析器移到 `vdhs_sym.c` / `vdhs_sym.h`，并新增 APK 内嵌库的回退：PICO runtime 的库直接从 `XRRuntime.apk` 映射，`/proc/self/maps` 里没有任何路径能对应到 `.so`。候选基址取自这些映射起始处的 ELF 头，且每次读取都用镜像自身的段做边界校验，因此陈旧或被截断的映射不会再让目标进程崩溃。
- 已在头显验证：VD 里手势不再生效、退出后立刻恢复、手追与手部透视不受影响。
- **已在同一台头显上验证 Virtual Desktop 1.34.22.0**：本仓库期望的 loader 偏移与托管名称本就是 1.34.22 的，因此该版本上所有 hook 都无偏移不符地安装成功（714096 B loader、`xrEnumerateInstanceExtensionProperties @ +0x385f4` 等），扩展注入仍为 16→19、`supportsHandTracking=1`、两只手都建了 hand tracker、aim 对双手都能分类。该版本上 PICO 自身的 active-input 查询可用（`xrGetActiveInputDeviceTypePico`，`r=0`），所以 `pvr.active.input_device` 属性只是不导出该接口的版本（1.34.18.0）的回退。

## 1.0.8 — 2026-10-18 — PICO 4 (A8110) 移植

- **PICO 4 支持**：本机无眼动/面部追踪硬件，从原生层与托管 mod 中移除相关代码。`vdhs_layer.c` 删除注视块（`hk_create_action_space`/`hk_locate_space`、Mono 栈回溯、One-Euro 滤波）、gaze hook 表项、`hk_create_action` 的 `eye_gaze_pose` 分支与注视权限修复；`vdhs_hand.c` 删除注视配置字段与 `eye_gaze` 还原路径。`module.prop` / `customize.sh` 标注 “PICO 4 slim: no eye/face tracking”。
- **输入判定回退**：PICO 4 运行时未导出任何 active-input-device 查询（各拼写均返回 `XR_ERROR_FUNCTION_UNSUPPORTED`）。层改为在 `vdhs_hand_active_input()` 读取系统属性 `pvr.active.input_device`（`2` = 双手），并据此把 grip 姿态的 `isActive` 置 0——这正是让 VD 切换到双手的关键。已在 VD 1.34.18.0 验证（`exts 16->19`、`supportsHandTracking=1`、`hand_ext=1`、`locate ... isActive=1`、VD 为两只手拉取手部网格）。SteamVR 双手显示与手势透视均正常。
- **打包**：打包含 Mono 兼容的 `0Harmony.dll`（`netstandard2.0` / `net472`）而不再用 `net35`——后者在头显的 Mono 上加载失败（`mscorlib, Version=2.0.0.0`）。ZIP 改用 .NET `ZipArchive` 并以正斜杠分隔（PowerShell 5.1 的 `Compress-Archive` 会写反斜杠）。
- 托管 `IsControllerActive` 切换（Harmony/redirect）在 PICO 4 上**非必需**；`auto` → `dataonly` 即可。

## 1.0.7 — 2026-10-10

- 为眼动消费者栈遍历补齐 Mono GC 线程状态的进入／退出配对，修正进入 SteamVR 时并发首次查询可能触发的 `STATE_BLOCKING / DO_BLOCKING` 崩溃路径。缺少线程状态 API 时跳过指针增强；串流眼动继续保留上游结果。
- 2026-10-10 在 PICO 4 Pro / VD 1.34.22.0、PicoET dual/on 下完成两次启动并进入 SteamVR 的实机观察，用户确认均未闪退且随后主动退出。其中一次日志确认 HMD-only 输入；两次首次并发眼动查询均完成分流，安装后的 crash buffer 无新增记录。尚未做长时间稳定性观察。

## 1.0.6 — 2026-10-08

- 默认开启全部已实现的用户功能，补齐 SteamVR 手部透视开孔（`pt_hole=1`）和 300 ms 手部关节冻结门控。开孔半径默认 0.1 m，与 VD 的手部窗口一致。
- 网格纳入 Magisk 模块 ZIP，Zygisk 在首次启动时自动放置缺失的网格；直接刷入模块也无需再手动复制。已有自定义网格保留，一键安装显式指定 `-MeshPath` 时可替换。
- 保留 `hand_gesture.txt` 中的显式设置；SteamVR 手透继续跟随 VD 自身开关，不强改应用设置。调试日志、纯 alpha 诊断与 `pt_style` A/B 诊断默认关闭。
- 托管与原生构建无警告，模块打包、shell／PowerShell 语法检查与公开内容检查通过。
- 本轮默认值及自动网格安装尚未完成头显真机验证。

## 1.0.5 — 2026-10-03

- 推送版本标签后，由 GitHub Actions 自动构建、打包并发布整合包。Release 仅发布 `VD-PicoEnhance-v<版本号>.zip`，安装脚本、模块 ZIP、手部网格、许可说明和校验文件均包含在包内。
- 安装脚本帮助和文档同步使用整合包下载方式。

## 1.0.4 — 2026-10-03

### 修复

- **注视点串流**：将模块的眼动平滑、跳变确认、无效样本保持及头部姿态补偿限定到界面指针调用。串流、追踪转发和未识别调用保留上游姿态及有效标志，滤波历史改为线程独立。
- **启动内存越界**：为新增的 hand 和 controller 扩展各预留一个槽位，修复扩展列表的堆越界写。
- **首次安装权限**：安装脚本设置应用载荷目录的所有者、权限和 SELinux 标签，使应用可以写入载荷。
- **Hook 部分失败**：核心与透视 hook 分别在整组成功后启用，失败时回滚；未启用的回调直接转交原函数。后台托管加载器和输入诊断不再在安装失败时尝试 runtime 协商。
- **手部射线**：仅采用同时具有 computed、valid 标志且姿态数值有效的 runtime 射线，否则使用合成射线，避免继续使用已失效的旧姿态。
- **捏合配置**：将示例阈值纠正为 `0.4/0.8/3.0` cm，并说明强度公式。校验数值有限且 `0 ≤ on < off ≤ full`，无效配置整组恢复默认值。
- **安装失败处理**：检查每一步 ADB 调用的退出码，远端操作失败时停止后续步骤，避免使用传输失败后残留的旧文件。
- **旧载荷残留**：构建时移除未选用的旧 redirect DLL，注入时清理模块已不再提供的可选 DLL 和配置。载荷文件先写入临时文件再原子替换，必需文件缺失或复制失败时跳过注入。

### 调整

- 界面眼动指针的头部姿态补偿默认开启（`gaze_vd_fix=1`），眼动平滑继续默认开启。两项设置均只影响指针。
- 更新日志分别提供中英文文件，由 README 链接进入；修正手动安装网格的权限设置。
- 新增整合包 `VD-PicoEnhance-v1.0.4.zip`，包含安装脚本、模块 ZIP、手部网格、许可说明和校验文件，一次下载即可解压安装。

### 升级

此轮更新包含 Zygisk 注入器，请安装完整模块并重启头显。安装脚本保留 `hand_gesture.txt`；已有的 `gaze_vd_fix=0` 会继续关闭补偿，改为 `1` 或删除该行即可使用新默认值。

如果继续使用 v1.0.3，串流时请把 `gaze_vd_fix` 和 `gaze_filter` 都设为 `0`，然后重启 VD。该版本会将指针处理同时应用到串流眼动。

### 实机观察

2026-10-02 在 PICO 4 Pro / VD 1.34.22.0 上完成修复构建的一轮功能观察：手／手柄切换、捏合与射线恢复、双手透视、眼动和重新启动均正常。当时头部补偿已开启（`gaze_vd_fix=1`），眼动平滑使用默认值 `1`。日志确认指针增强生效、串流眼动跳过增强，读取的 crash buffer 为空。

## 1.0.3 — 2026-10-02

- 安装脚本改为使用同目录下的本地 ZIP 和网格文件；缺少文件时退出，不再自动下载。

## 1.0.2 — 2026-10-02

- 新增一键安装脚本，并随 Release 提供；支持安装模块、放置手部网格和重启头显。

## 1.0.1 — 2026-10-02

- 默认启用完整兼容层（`mode=2`），补充手部网格安装说明。

## 1.0.0 — 2026-10-02

- 首个公开版本，提供 PICO 手追、手／手柄切换、手部透视与眼动指针处理，以及构建和 Release 工作流。
