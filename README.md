# VD-PicoEnhance

English | [简体中文](README_zh.md)

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: PICO 4 Pro](https://img.shields.io/badge/platform-PICO%204%20Pro-lightgrey)

Bring **PICO optical hand tracking**, **Quest-style hand passthrough** and
**eye-gaze fixes** to the **unmodified official Virtual Desktop** app
(`VirtualDesktop.Android`) on **PICO 4 Pro**.

VD-PicoEnhance is a Magisk/Zygisk module that injects a small OpenXR
compatibility layer into the Virtual Desktop process at runtime. The Virtual
Desktop APK is **not** modified, re-signed, renamed or repacked — the module only
makes the app use PICO features that the vendor's own loader never asks for.

> ⚠️ **Experimental, root-only and version-specific.** The module hooks a
> closed-source commercial application. It needs root, and a Virtual Desktop
> update can break it. Read [Requirements](#requirements),
> [Known issues](#known-issues) and [Disclaimer](#disclaimer) before installing.
> This is an independent community project, not affiliated with Virtual Desktop
> or PICO.

## Features

| Feature | What it does | Toggle (`hand_gesture.txt`) |
|---|---|---|
| Hand tracking | Makes Virtual Desktop see `XR_EXT_hand_tracking` and injects it into the app's managed extension list | always on |
| `XR_FB_hand_tracking_aim` | Synthesises pinch / menu gestures and a pointing ray; the runtime's own aim wins when it is valid | `aim_*`, `pinch_*`, `menu_*` |
| `XR_FB_hand_tracking_mesh` | Serves a hand mesh from `hand_mesh_fb.bin` (shipped in releases, or generated from your own headset) | enabled when the file exists |
| Hand/controller hot-switch | Put a controller down to switch to hands, pick it up to switch back | — |
| Hand passthrough (in Virtual Desktop) | One projected layer per hand; restores the background passthrough when paused | `pt_split`, `pt_bg_fix` |
| Hand passthrough (in SteamVR) | Writes an alpha hole in place on Virtual Desktop's own stream image, only where the hands are — no copy, no extra layer | `pt_hole`, `pt_follow_settings` |
| Freeze gate | Reports a hand inactive when its joints stop updating (PICO hand "flash") | `hj_freeze_ms` |
| Eye gaze | One-Euro smoothing, jump confirmation, and compensation for the head pose Virtual Desktop drops on PICO | `gaze_filter`, `gaze_vd_fix` |

In SteamVR a hand-passthrough hole is only opened when Virtual Desktop's own
`PassthroughPortals` logic would show hand passthrough (VR stream source, VR
passthrough hands enabled, hands as current input, ...). Outside SteamVR no layer
is touched.

## Requirements

| Component | Requirement |
|---|---|
| Headset | **PICO 4 Pro**, arm64-v8a |
| Root | Magisk with **Zygisk enabled** (Zygisk API v5, i.e. Magisk ≥ 27000) |
| Virtual Desktop | Android build `VirtualDesktop.Android` **1.34.22.0**, unmodified |
| Host (to build) | Windows + PowerShell, Android **NDK 26.1.10909125**, **CMake 3.22.1**, .NET SDK |

> Other Virtual Desktop versions are untested. The layer resolves everything
> through the app's in-memory symbols and the PICO runtime, but internal method
> names are version-specific.

## How it works

A Zygisk module targets the Virtual Desktop process, stages a native payload
(`libvdhs.so`) and a managed mod (`VdHsMod.dll`) into the app's private data
directory, and installs **inline hooks on Virtual Desktop's own bundled OpenXR
loader**. Through those hooks the layer reaches the PICO runtime, enables the
PICO hand-tracking extensions, synthesises the FB aim/mesh data, and writes the
hand-passthrough hole. No system library and no installed file is modified.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full design, the hook
list, the managed backends and the robustness contract.

## Repository layout

| Path | Contents |
|---|---|
| `mod/native/` | Native payload `libvdhs.so`: `vdhs_payload.c` (Mono embedding loader), `vdhs_layer.c` (OpenXR hooks, gaze), `vdhs_hand.c` (hand tracking, aim/mesh), `vdhs_pt.c` (SteamVR passthrough hole), `vdhs_mark.c`; vendored `third_party/shadowhook-2.0.1/` |
| `mod/src/VdHsMod/` | Managed mod (netstandard2.0, reflection only, no Virtual Desktop assembly references) |
| `zygisk/` | Magisk/Zygisk module: injector `jni/`, `payload/` (`mode.txt`, `backend.txt`), `build.ps1` |
| `tools/mesh/` | Convert the headset's own PICO hand mesh into `hand_mesh_fb.bin` |
| `assets/mesh/` | Prebuilt hand mesh blob shipped in releases, with its provenance and disclaimer |
| `config/` | Example `hand_gesture.txt` with all runtime toggles |
| `docs/` | Architecture notes |

## Build

Point `ANDROID_SDK_ROOT` (or `ANDROID_HOME`) at an Android SDK that has NDK
26.1.10909125 and CMake 3.22.1 installed (or write the path to the untracked
`tools/sdk-path.txt`), then:

```powershell
# Native payload + managed mod + Zygisk injector, staged into zygisk/payload/
pwsh -File zygisk/build.ps1

# Also produce dist/vdhs_zygisk.zip (the Magisk install package)
pwsh -File zygisk/build.ps1 -Pack
```

The native payload requires zero warnings; the default managed build restores
from nothing and compiles offline.

## Install

1. In Magisk: **Modules → Install from storage**, pick `dist/vdhs_zygisk.zip`,
   then reboot.
2. The shipped `payload/mode.txt` is `1`, which only proves injection. Confirm
   the module works, then set
   `/data/adb/modules/vdhs_zygisk/payload/mode.txt` to `2` and restart Virtual
   Desktop for the full layer.
3. To update just the payload later (no full reboot), push and copy
   `libvdhs.so` into `/data/adb/modules/vdhs_zygisk/payload/`, then restart
   Virtual Desktop.

> PICO's gesture mode can block Virtual Desktop's cold start. Put a controller
> down and wait for `getprop sys.pxr.trackingservice.gesturemode` to read `0`
> before launching Virtual Desktop.

## Configuration

Copy [`config/hand_gesture.example.txt`](config/hand_gesture.example.txt) to
`/data/data/VirtualDesktop.Android/vdhs/hand_gesture.txt` on the headset and
adjust the keys you need. Every key is optional; missing keys keep their
built-in default. Current settings and the full key list are in that file.

### Hand mesh

The hand mesh (`XR_FB_hand_tracking_mesh`) is served from `hand_mesh_fb.bin`
placed next to the payload; the mesh extension is simply unavailable until you
provide it.

- **Prebuilt blob**: releases ship `hand_mesh_fb.bin`, converted from the
  headset's own PICO system resource (`XRShell.apk`) with
  `tools/mesh/xrshell_mesh.py`. Push it to
  `/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin`. Provenance and
  disclaimer: [`assets/mesh/README.md`](assets/mesh/README.md).
- **Build it yourself** (with numpy installed):

  ```sh
  python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
  # then push out/hand_mesh_fb.bin to <app_data>/vdhs/hand_mesh_fb.bin
  ```

`hand_mesh_fb.bin` is a PICO system asset, included only to make the
interoperability feature work and unaffiliated with PICO; it will be removed on
request.

## Known issues

- **Pinch to switch the main hand in SteamVR does not work.** With the hand
  open, the trigger reads about 0.27 and never 0; Virtual Desktop only emits a
  digital click at exactly 1.0. A trigger dead-zone/remap in the managed layer is
  the likely fix, untested.
- **Eye gaze** still drifts a little while turning the head.
- The runtime (or SteamVR) occasionally freezes hand joints for a long time; the
  hand gesture then "flashes" and disappears until SteamVR is restarted.
- PICO shows a "hand gestures not supported" prompt in the headset; not handled.
- Virtual Desktop updates can change internal names and break the layer until
  it is updated.

## Uninstall / rollback

- Quick rollback: set `payload/mode.txt` to `1` and restart Virtual Desktop —
  the layer is skipped entirely.
- Full removal: disable/remove the module in Magisk and reboot:
  ```sh
  touch /data/adb/modules/vdhs_zygisk/disable   # temporary
  rm -rf /data/adb/modules/vdhs_zygisk          # remove completely
  ```
- Optional cleanup of the staged payload: delete
  `<app_data>/vdhs/` (i.e. `/data/data/VirtualDesktop.Android/vdhs/`).

## Privacy and security

- The module needs **root** and injects code into Virtual Desktop. Only install
  it on a device you control, and only from a build you produced or trust.
- The injector stages files into the app's private data directory and does not
  write anywhere else. It does not touch the APK, the manifest, the signature or
  the package name, and does not modify other processes.
- No Virtual Desktop, PICO, Xenko or Mono binaries are bundled here; see
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Issue reports: include headset model, PICO OS, Virtual Desktop version and a
  de-identified log excerpt. Do not upload device identifiers or signing keys.

## Disclaimer

This is an unofficial, experimental community project. It does not contain,
redistribute or bypass Virtual Desktop, PICO or their licenses, and it does not
modify any installed application. Using a root module against a third-party app
is done **at your own risk**: it may break Virtual Desktop or your device, and
you are responsible for complying with the terms of the software you own.
"Virtual Desktop" and "PICO" are the property of their respective owners and are
referenced here only to describe interoperability.

## Acknowledgements

- [ShadowHook](https://github.com/bytedance/android-inline-hook) for the inline
  hooking runtime.
- John "topjohnwu" Wu for the Magisk/Zygisk module API.
- The `hand_mesh_fb.bin` tool build on the PICO system hand mesh, converted with
  the offline helper in `tools/mesh/`.

## License

[MIT](LICENSE). Third-party components remain under their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
