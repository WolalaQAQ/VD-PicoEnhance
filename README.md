# VD-PicoEnhance

English | [简体中文](README_zh.md)

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: PICO 4 Pro](https://img.shields.io/badge/platform-PICO%204%20Pro-lightgrey)

Brings **PICO optical hand tracking**, **Quest-style hand passthrough** and an **eye-gaze pointer fix** to the **unmodified official** Virtual Desktop (`VirtualDesktop.Android`) on the **PICO 4 Pro**.

VD-PicoEnhance is a Magisk/Zygisk module that injects a small OpenXR compatibility layer into the Virtual Desktop process at runtime. It does not modify, repack, re-sign or rename the Virtual Desktop APK; it only lets the app use PICO features that its bundled loader never asks for.

> Experimental, requires root, and tied to the Virtual Desktop version. The module hooks a closed-source commercial app and may break when Virtual Desktop updates. Read [Requirements](#requirements), [Known issues](#known-issues) and [Disclaimer](#disclaimer) before installing. This is an independent community project, not affiliated with Virtual Desktop or PICO.

## Updates

See the [changelog](CHANGELOG.md) for changes in each version.

## Features

| Feature | How | Switches (`hand_gesture.txt`) |
|---|---|---|
| Hand-tracking extension | Makes Virtual Desktop see `XR_EXT_hand_tracking` and injects the extension into the app's managed extension list | always on |
| `XR_FB_hand_tracking_aim` | Synthesises pinch, menu gesture and pointing ray; the runtime's own aim is preferred when valid | `aim_*`, `pinch_*`, `menu_*` |
| `XR_FB_hand_tracking_mesh` | Serves a hand mesh from `hand_mesh_fb.bin` (shipped with releases, or generated from your own headset) | enabled when the file exists |
| Hand/controller hot-switch | Put the controller down to use hands, pick it up to go back | — |
| Hand passthrough (in Virtual Desktop) | One projected layer per hand; the background passthrough state is restored when no hole is drawn | `pt_split`, `pt_bg_fix` |
| Hand passthrough (in SteamVR) | Sets the alpha in place on Virtual Desktop's streamed swapchain, opening a hole only where a hand is; no copy, no extra layer | `pt_hole`, `pt_follow_settings` |
| Joint freeze gate | Reports a hand as inactive when its joints have not updated for a while, easing the "hand flashes and disappears" behaviour | `hj_freeze_ms` |
| Eye gaze | Pointer-only One-Euro smoothing, jump confirmation and head-pose compensation, enabled by default; streaming gaze passes through | `gaze_filter`, `gaze_vd_fix` |

In SteamVR the hole follows the same conditions as Virtual Desktop's own `PassthroughPortals` logic (VR stream source, `VRPassthroughHands` enabled, hands as current input, and so on). Outside SteamVR the module does not touch any layer.

## Requirements

| Item | Requirement |
|---|---|
| Headset | **PICO 4 Pro**, arm64-v8a |
| Root | Magisk with **Zygisk enabled** (Zygisk API v5, i.e. Magisk ≥ 27000) |
| Virtual Desktop | Android `VirtualDesktop.Android` **1.34.22.0**, unmodified |
| PC (build only) | Windows + PowerShell, Android **NDK 26.1.10909125**, **CMake 3.22.1**, .NET SDK |

Other Virtual Desktop versions are untested. The compatibility layer works through symbols in the app's memory and the PICO runtime's resolution interface, but internal method names are version-specific.

## How it works

The Zygisk module matches the Virtual Desktop process, stages the native payload `libvdhs.so` and the managed mod `VdHsMod.dll` into the app's private directory, and installs inline hooks on the exports of **Virtual Desktop's own OpenXR loader**. Through those hooks the layer reaches the PICO runtime: it requests the PICO hand-tracking extensions, synthesises FB aim/mesh data and writes the hand-passthrough hole. No system library or installed file is changed.

For the full component breakdown, the hook list, the managed backends and the robustness contract, see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Repository layout

| Path | Contents |
|---|---|
| `mod/native/` | Native payload `libvdhs.so`: `vdhs_payload.c` (Mono embedding loader), `vdhs_layer.c` (OpenXR hooks, gaze), `vdhs_hand.c` (hand tracking, aim/mesh), `vdhs_pt.c` (SteamVR passthrough hole), `vdhs_mark.c`; vendored `third_party/shadowhook-2.0.1/` |
| `mod/src/VdHsMod/` | Managed mod (netstandard2.0, reflection only, no reference to Virtual Desktop assemblies) |
| `zygisk/` | Magisk/Zygisk module: injector `jni/`, `payload/` (`mode.txt`, `backend.txt`), `build.ps1` |
| `tools/mesh/` | Converts the headset's own PICO hand mesh into `hand_mesh_fb.bin` |
| `assets/mesh/` | The hand mesh blob shipped with releases, with provenance and disclaimer |
| `config/` | `hand_gesture.txt` example with every runtime switch |
| `docs/` | Architecture notes |

## Building

Point `ANDROID_SDK_ROOT` (or `ANDROID_HOME`) at an Android SDK that has NDK 26.1.10909125 and CMake 3.22.1, or write the SDK path into the untracked `tools/sdk-path.txt`. Then:

```powershell
# native payload + managed mod + Zygisk injector, staged into zygisk/payload/
pwsh -File zygisk/build.ps1

# additionally pack dist/vdhs_zygisk.zip (the Magisk module)
pwsh -File zygisk/build.ps1 -Pack
```

The native payload must build with zero warnings; the default managed build restores no NuGet packages and compiles offline.

## Installation

### One-shot installer (recommended)

Download `VD-PicoEnhance-v<version>.zip` from [Releases](https://github.com/WolalaQAQ/VD-PicoEnhance/releases/latest) and extract it on the PC. The installer, module ZIP and hand mesh will be in the same directory. Open PowerShell there and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1
```

The script finds the headset over adb, checks root, installs the local module ZIP, places the local mesh in the right directory, and reboots the headset. It looks beside the script, regardless of the terminal's working directory; it never downloads files and stops if either file is missing. The headset must be rooted (Magisk + Zygisk) with USB debugging enabled.

Useful parameters: `-Mode 1` for an injection-only control run, `-NoReboot` to skip the automatic reboot, `-ZipPath` / `-MeshPath` to specify local files elsewhere, and `-Serial` to pick a device when several are connected. To install a different version, use the bundle from that release.

### Manual installation

1. Extract the bundle. In Magisk choose **Modules → Install from storage**, select the included `vdhs_zygisk.zip`, then reboot the headset.
2. Launch Virtual Desktop once, install `hand_mesh_fb.bin` as described under [Hand mesh](#hand-mesh), then restart the app. The one-shot installer includes mesh installation.

`payload/mode.txt` defaults to `2`, the full layer. For a control run that only injects and does not load the managed layer, change `/data/adb/modules/vdhs_zygisk/payload/mode.txt` to `1` and restart Virtual Desktop.

To upgrade, run the installer from the new bundle to install the full module, then reboot the headset.

> PICO blocks a Virtual Desktop cold start while it is in gesture mode. Pick up the controller, wait until `getprop sys.pxr.trackingservice.gesturemode` returns `0`, then start Virtual Desktop.

## Configuration

Copy [`config/hand_gesture.example.txt`](config/hand_gesture.example.txt) to `/data/data/VirtualDesktop.Android/vdhs/hand_gesture.txt` on the headset and adjust it as needed. Every key is optional; a missing key keeps its built-in default. The example lists all key names with the currently recommended values.

For SteamVR hand passthrough, set `pt_hole=1` and enable VD's VR hand-passthrough setting. UI gaze smoothing and head-pose compensation are enabled by default; explicit values in an existing configuration override the defaults. Restart VD after saving changes.

### Hand mesh

`XR_FB_hand_tracking_mesh` needs `hand_mesh_fb.bin` next to the payload, in the app's private directory:

```text
/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin
```

Without this file, the module does not enable VD's hand-mesh feature. The directory already exists once the module has run.

**Use the blob from a release (recommended).** The one-shot installer places it automatically. For manual installation, run on the PC:

```sh
adb push hand_mesh_fb.bin /data/local/tmp/
adb shell
```

In the device shell, enter `su` to obtain root, then run:

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

Restart Virtual Desktop afterwards. You can also copy it there with a root file manager on the headset.

**Generate your own** (requires numpy):

```sh
python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
# then copy out/hand_mesh_fb.bin in as above
```

Provenance and disclaimer: [`assets/mesh/README.md`](assets/mesh/README.md).

## Known issues

- **Pinch (trigger) as a main-hand switch does not work in SteamVR.** With the hand open the trigger reads about 0.27 and never reaches 0, while Virtual Desktop only emits a digital click at exactly 1.0. A candidate fix is to add a dead zone and remap the trigger in the managed layer; untested.
- **Eye gaze** still drifts slightly when you turn your head.
- The runtime (or SteamVR) sometimes freezes the hand joints for a long time; the gesture then "flashes" and disappears. Restarting SteamVR recovers it.
- The headset shows PICO's "gestures not supported" prompt; not handled for now.
- A Virtual Desktop update can change internal names and break the layer until it is adapted.

## TODO

- [ ] **USB wired streaming**: Virtual Desktop 1.34.22 supports wired streaming over USB NCM, but PICO does not currently expose that link to apps, so the app's USB mode is unavailable there. The plan is to bring up a USB network interface on the headset and let the compatibility layer use it, validating the link and discovery on-device before deciding whether to ship it.
- [ ] **Controller battery level**: pass the PICO controller battery level through to SteamVR, so the remaining charge is visible in SteamVR and in games.

## Uninstall and rollback

- Quick rollback: set `payload/mode.txt` back to `1` and restart Virtual Desktop; the compatibility layer is skipped entirely.
- Full removal: disable or remove the module in Magisk and reboot.

  ```sh
  touch /data/adb/modules/vdhs_zygisk/disable   # disable temporarily
  rm -rf /data/adb/modules/vdhs_zygisk          # remove completely
  ```

- Optionally clean the staged payload: delete `<app_data>/vdhs/`, that is `/data/data/VirtualDesktop.Android/vdhs/`.

## Privacy and security

- The module needs **root** and injects code into Virtual Desktop. Use it only on your own device and only with a build you made yourself or trust.
- The injector stages files into the app's private directory and nothing else. It does not change the APK, manifest, signature or package name, and does not touch other processes.
- This repository bundles no Virtual Desktop, Xenko, Mono or PICO binaries or sources, with the single exception of the hand mesh blob shipped as a separate release asset; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- When opening an issue, include the headset model, PICO OS version, Virtual Desktop version and sanitized log excerpts. Do not upload device identifiers or signing keys.

## Disclaimer

This is an unofficial, experimental community project. It does not contain, forward or bypass Virtual Desktop, PICO or their licenses, and it modifies no installed app. Using a root module against a third-party app is at your own risk: it can break Virtual Desktop or the device, and you are responsible for complying with the terms of the software you use. "Virtual Desktop" and "PICO" belong to their respective owners and are used here only to describe interoperability.

## Acknowledgements

- [ShadowHook](https://github.com/bytedance/android-inline-hook): the inline-hook runtime.
- John "topjohnwu" Wu: the Magisk / Zygisk module API.
- `hand_mesh_fb.bin` is derived from the PICO system hand mesh and converted with the offline tooling in `tools/mesh/`.

## License

[MIT](LICENSE). Third-party components remain under their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
