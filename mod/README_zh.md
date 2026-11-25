# `mod/` — 托管 Mod（`VdHsMod`）与原生载荷

[English](README.md) | **简体中文**

`mod/` 包含运行时暂存进 Virtual Desktop 的原生 C 载荷（`libvdhs.so` 与 `libvdhs_shell.so`）以及托管 C# mod（`VdHsMod.dll`）。

`VdHsMod` 编译目标为 `netstandard2.0`，对任何 Virtual Desktop、Xenko 或 `Mono.Android` 程序集保持**零编译期引用**——所有类型、成员和事件均在运行时按名称通过反射解析。

## 目录结构

```text
mod/
├── src/VdHsMod/                  C# 托管 mod（netstandard2.0，默认离线编译）
│   ├── Loader.cs                 libvdhs.so 调用的入口类
│   ├── Backends.cs               后端选择（auto / redirect / dataonly / none）
│   ├── ExtensionEnabler.cs       托管 OpenXR 扩展列表注入
│   ├── GestureToggle.cs          运行时“屏蔽系统手势”复选框与 UI 调度
│   ├── UiLocalize.cs             简体中文界面与性能面板自动汉化
│   ├── StreamingLimits.cs        3 倍码率与高分辨率画质滑块解锁
│   ├── RedirectBackend.cs        可选预修补程序集预加载后端
│   ├── AssemblyRedirect.cs       redirect 后端的程序集解析挂钩
│   ├── PayloadConfig.cs          读取 backend.txt / VDHS_BACKEND
│   ├── VdReflection.cs           反射辅助工具
│   ├── NativeBridge.cs           托管到原生的桥接层
│   └── VdHsMod.csproj
├── native/                       原生 C11 载荷（Android NDK + CMake / Ninja 构建）
│   ├── vdhs_payload.c            Mono 嵌入加载器（vdhs_payload_main）
│   ├── vdhs_layer.c              OpenXR loader inline hook（ShadowHook）
│   ├── vdhs_hand.c               手追、FB aim / mesh、关节冻结门控、Mono 助手
│   ├── vdhs_pt.c                 SteamVR 零拷贝手部透视 alpha 开孔
│   ├── vdhs_mark.c               xrMarkApiClass(0x3b) runtime 直达调用
│   ├── vdhs_battery.c            手柄电量 OpenXR action 与采样
│   ├── vdhs_caps.c               运行时眼动 / 面捕能力门控
│   ├── vdhs_sym.c                xDL 已映射符号解析适配器（两个 .so 共用）
│   ├── vdhs_shell.c              PICO Shell 手势屏蔽接收端（libvdhs_shell.so）
│   ├── third_party/shadowhook-2.0.1/  内置 ShadowHook 2.0.1 与 xDL
│   └── CMakeLists.txt
└── tools/
    └── bump_mvid.cs              为本地 redirect 产物更新 MVID 的辅助脚本
```

## 构建方式

```powershell
# 仅构建托管 mod（默认离线安全，不还原 NuGet 包）
dotnet build mod/src/VdHsMod/VdHsMod.csproj -c Release

# 构建 VdHsMod.dll、libvdhs.so、libvdhs_shell.so、启动助手与 Zygisk 注入器
pwsh -File zygisk/build.ps1

# 构建并打包 zygisk/dist/vdhs_zygisk.zip
pwsh -File zygisk/build.ps1 -Pack
```

`zygisk/build.ps1` 以 `-Wall -Wextra` 编译全部原生目标（要求零警告），并将 `libvdhs.so`、`libvdhs_shell.so`、`VdHsMod.dll` 以及内置的 `hand_mesh_fb.bin` 暂存到 `zygisk/payload/`。其他参数：

- `-Clean`：配置前清理 CMake 构建目录。
- `-Harmony`：启用旧版 Harmony 后端构建（`-p:VdHsHarmony=true`，从 NuGet 还原 `Lib.Harmony 2.3.5` 并暂存 `0Harmony.dll`）。
- `-RedirectArtifact <路径>`：将本地构建的 `Xenko.OpenXR.patched.dll` 暂存到 `zygisk/payload/`。

## 托管后端切换

无需重新编译，编辑 `zygisk/payload/backend.txt`（或设置环境变量 `VDHS_BACKEND`）后重启 Virtual Desktop 即可切换后端：

| 后端 | 行为 | 说明 |
|---|---|---|
| `auto` | 若已暂存 `Xenko.OpenXR.patched.dll` 则使用 `redirect`，否则若已编译并暂存 Harmony 则使用 `harmony`，否则回落到 `dataonly` | 默认值 |
| `redirect` | 预加载本地构建并更新过 MVID 的 `Xenko.OpenXR.patched.dll`，由 Mono JIT 执行而非命中 APK 原带 AOT 镜像 | 实验性，需自行构建 |
| `dataonly` | 纯反射数据注入，启用 PICO 扩展但不改动 `IsControllerActive` | 默认回退方案 |
| `none` | 跳过后端改动；完整模式下原生 OpenXR 兼容层和托管 UI 功能依然运行 | 不是注入对照模式——对照测试请将 `payload/mode.txt` 改为 `1` 并重启 |

## 生成本地 Redirect 产物（可选）

`redirect` 后端是可选的实验路线，正式发布的 Zygisk 兼容层并不依赖它。如需把本地修改过的 `Xenko.OpenXR.dll` 转为不匹配官方 AOT 镜像的 redirect 产物：

```powershell
# 变更 MVID，使 Mono 以 JIT 方式加载替换程序集
dotnet run mod/tools/bump_mvid.cs -- <patched.dll> mod/redirect/Xenko.OpenXR.patched.dll

# 暂存到模块目录树
pwsh -File zygisk/build.ps1 -RedirectArtifact mod/redirect/Xenko.OpenXR.patched.dll
```

`mod/redirect/` 为构建输出目录，不纳入版本控制。
