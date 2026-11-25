# 手部网格数据文件（`hand_mesh_fb.bin`）

[English](README.md) | **简体中文**

`hand_mesh_fb.bin` 为 VD-PicoEnhance 的 `XR_FB_hand_tracking_mesh`（`xrGetHandMeshFB`）提供手部几何数据。运行时若缺少有效文件，模块不会向 Virtual Desktop 启用手部网格功能。

## 文件格式

这是一个自描述的小端二进制文件（`VDHM` 容器，版本 `1`，带 CRC32 校验，所有数组按 16 字节对齐），保存左右手数据并可直接复制到 OpenXR `XrHandTrackingMeshFB` 缓冲区：

- 26 个 OpenXR 关节绑定姿态、半径和父级索引（按 `XrHandJointEXT` 顺序）
- 已转换到 OpenXR / FB 坐标系与绕序约定的顶点位置、法线、UV、蒙皮权重 / 骨骼索引和三角形索引

完整二进制布局定义见 [`tools/mesh/xrshell_mesh.py`](../../tools/mesh/xrshell_mesh.py) 文件头部注释。

## 来源与完整性校验

该文件**转换自头显自带的 PICO 系统资源**，并非本项目原创绘制：

1. **原始来源**：`/system/priv-app/XRShell/XRShell.apk`（PICO `XRShell` 系统应用）内的 `assets/mesh/HAND_{L,R}_OpenXR.MESH/`。
2. **转换方式**：在 PC 上通过 `tools/mesh/xrshell_mesh.py blob` 离线生成，构建期与运行期均不链接任何 PICO 库。

```text
sha256  bc63e23093b8184b510219e78dbe2b67939e87362eab092c634edf2f6e6fca27
size    187200 bytes
```

Release 整合包附带完全一致的该文件，并在 `SHA256SUMS.txt` 中记录同一校验值。

## 安装与替换

运行时兼容层从 Virtual Desktop 私有目录读取该文件：

```text
/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin
```

Zygisk 模块（首次启动 VD 时）和 `install-vd-picoenhance.ps1` 均会在文件缺失时自动放置内置网格，同时保留已有网格文件。

### 通过安装器替换

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -MeshPath .\hand_mesh_fb.bin
```

### 通过 ADB 手动替换

关闭 Virtual Desktop，在 PC 上推送文件并以 root 设置属主与 SELinux 标签：

```sh
adb push hand_mesh_fb.bin /data/local/tmp/
adb shell
```

在设备 shell 中执行 `su` 获取 root 后运行：

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

`vdhs` 目录必须保持归属应用 UID，注入器下次启动时才能正常暂存载荷。替换后重启 Virtual Desktop 即可生效。

### 从自己的头显自行生成

安装 Python 3 与 `numpy`，从你的头显提取 `/system/priv-app/XRShell/XRShell.apk` 后执行：

```sh
python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
```

随后按上述任一方式安装生成的 `out/hand_mesh_fb.bin`。

## 再分发免责声明

`XRShell.apk` 及其手部网格资源归 PICO 所有。此处附带转换后的文件仅用于让未修改的第三方应用开箱即用地支持手部网格互操作特性，与 PICO 无任何关联或背书；如版权方提出要求，本项目将立即移除。你也可以用 `tools/mesh/xrshell_mesh.py` 从自己的设备生成副本替换，或同时删除 `/data/adb/modules/vdhs_zygisk/payload/hand_mesh_fb.bin` 与 `/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin` 以无网格模式运行。
