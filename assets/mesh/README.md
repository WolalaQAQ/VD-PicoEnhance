# Hand Mesh Blob (`hand_mesh_fb.bin`)

**English** | [简体中文](README_zh.md)

`hand_mesh_fb.bin` supplies the hand geometry that VD-PicoEnhance serves through `XR_FB_hand_tracking_mesh` (`xrGetHandMeshFB`). Without a valid file at runtime, Virtual Desktop's hand-mesh feature is left disabled.

## Format

A self-describing little-endian binary (`VDHM` container, version `1`, CRC32-checked, 16-byte aligned arrays) storing left and right hand data ready for direct copy into OpenXR `XrHandTrackingMeshFB` buffers:

- 26 OpenXR joint bind poses, radii, and parent indices (`XrHandJointEXT` order)
- Vertex positions, normals, UVs, skinning blend indices/weights, and triangle indices transformed into OpenXR/FB coordinate and winding conventions

The complete binary layout is documented at the top of [`tools/mesh/xrshell_mesh.py`](../../tools/mesh/xrshell_mesh.py).

## Provenance & Integrity

This file is **converted from the headset's own PICO system resource**, not authored by this project:

1. **Source**: `assets/mesh/HAND_{L,R}_OpenXR.MESH/` inside `/system/priv-app/XRShell/XRShell.apk` (PICO's `XRShell` system app).
2. **Conversion**: Generated offline on a host PC via `tools/mesh/xrshell_mesh.py blob`. No PICO library is linked at build time or runtime.

```text
sha256  bc63e23093b8184b510219e78dbe2b67939e87362eab092c634edf2f6e6fca27
size    187200 bytes
```

Releases publish this exact blob and record its checksum in `SHA256SUMS.txt`.

## Installation & Replacement

At runtime, the compatibility layer reads the mesh from Virtual Desktop's private directory:

```text
/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin
```

Both the Zygisk module (on first launch) and `install-vd-picoenhance.ps1` copy the bundled mesh automatically if missing, while preserving any existing file.

### Replace via Installer

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -MeshPath .\hand_mesh_fb.bin
```

### Replace Manually via ADB

Close Virtual Desktop, push the file from your PC, and set ownership and SELinux labels as root:

```sh
adb push hand_mesh_fb.bin /data/local/tmp/
adb shell
```

Inside the headset shell, elevate with `su` and run:

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

The `vdhs` directory must remain owned by the app UID so the injector can stage payloads on subsequent launches. Restart Virtual Desktop afterward.

### Generate from Your Own Headset

Install Python 3 with `numpy`, pull `/system/priv-app/XRShell/XRShell.apk` from your headset, and run:

```sh
python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
```

Install `out/hand_mesh_fb.bin` using either method above.

## Redistribution Disclaimer

`XRShell.apk` and its hand mesh are the property of PICO. This converted blob is included solely so the hand-mesh interoperability feature of an unmodified third-party application works out of the box, with no affiliation with or endorsement by PICO, and will be removed upon request. You may replace it with a copy generated from your own device using `tools/mesh/xrshell_mesh.py`, or run without a hand mesh by removing both `/data/adb/modules/vdhs_zygisk/payload/hand_mesh_fb.bin` and `/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin`.
