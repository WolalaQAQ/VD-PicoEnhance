# VD-PicoEnhance

English | [简体中文](README_zh.md)

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: PICO 4 / PICO 4 Pro](https://img.shields.io/badge/platform-PICO%204%20%2F%20PICO%204%20Pro-lightgrey)

Brings **PICO optical hand tracking** and **Quest-style hand passthrough** to the **unmodified official** Virtual Desktop (`VirtualDesktop.Android`) on **PICO 4 and PICO 4 Pro**, with an **eye-gaze pointer fix** on supported hardware.

VD-PicoEnhance is a Magisk/Zygisk module that adds these features at runtime while keeping the official Virtual Desktop APK unchanged. Full mode also loads a gesture receiver into PICO's Shell; suppression stays off until enabled in VD. On supported firmware, a startup helper runs in `system_server` so VD can launch in hand mode.

> Experimental and requires root. Tested with Virtual Desktop 1.34.22.0; a VD update may require an updated module.

## Updates

This README describes the current source. Features under [Unreleased in the changelog](CHANGELOG.md#unreleased---2026-10-19) are not yet in a release bundle; build the source to try them. For a published bundle, use the documentation from its version tag.

## Features

| Feature | How | Switches (`hand_gesture.txt`) |
|---|---|---|
| Hand-tracking extension | Makes Virtual Desktop see `XR_EXT_hand_tracking` and injects the extension into the app's managed extension list | always on |
| `XR_FB_hand_tracking_aim` | Synthesises pinch, menu gesture and pointing ray; the runtime's own aim is preferred when valid | `aim_*`, `pinch_*`, `menu_*` |
| `XR_FB_hand_tracking_mesh` | Serves a hand mesh from `hand_mesh_fb.bin` (bundled in the module, or generated from your own headset) | enabled when the file exists |
| PICO hand/controller hot-switch | Put the PICO controller down to use hands, pick it up to go back | — |
| Hand-mode VD startup | Declare VD's hand-tracking capability to the system on supported PICO firmware | automatic in full mode |
| Hand passthrough (in Virtual Desktop) | One projected layer per hand; the background passthrough state is restored when no hole is drawn | `pt_split`, `pt_bg_fix` |
| Hand passthrough (in SteamVR) | Sets the alpha in place on Virtual Desktop's streamed swapchain, opening a hole only where a hand is; no copy, no extra layer | `pt_hole`, `pt_follow_settings` |
| Joint freeze gate | Reports a hand as inactive when its joints have not updated for a while, easing the "hand flashes and disappears" behaviour | `hj_freeze_ms` (default 300 ms) |
| Eye gaze | Pointer-only One-Euro smoothing, jump confirmation and head-pose compensation, enabled by default; streaming gaze passes through | `gaze_filter`, `gaze_vd_fix` |

In SteamVR the hole follows the same conditions as Virtual Desktop's own `PassthroughPortals` logic (VR stream source, `VRPassthroughHands` enabled, hands as current input, and so on). Outside SteamVR the module does not touch any layer.

Eye-gaze enhancements require runtime eye-tracking support; hand tracking and passthrough also work on ordinary PICO 4 without that hardware. The integration has been tested on PICO 4 Pro, and the contributor has confirmed it works on ordinary PICO 4 in [PR #2](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/2#issuecomment-6263664529).

## Requirements

| Item | Requirement |
|---|---|
| Headset | **PICO 4 or PICO 4 Pro**, arm64-v8a; eye-gaze features require eye-tracking hardware |
| Root | Magisk with **Zygisk enabled** (Zygisk API v5, i.e. Magisk ≥ 27000) |
| Virtual Desktop | Android `VirtualDesktop.Android` **1.34.22.0**, unmodified |
| PC (build only) | Windows + PowerShell, Android **NDK 26.1.10909125**, **CMake 3.22.1**, **platform 35**, **build-tools 35.0.0**, **JDK 17**, .NET SDK |

Other Virtual Desktop versions are untested. The compatibility layer works through symbols in the app's memory and the PICO runtime's resolution interface, but internal method names are version-specific.

## How it works

The Zygisk module matches the Virtual Desktop process, stages the native payload `libvdhs.so` and the managed mod `VdHsMod.dll` into the app's private directory, and installs inline hooks on the exports of **Virtual Desktop's own OpenXR loader**. Through those hooks the layer reaches the PICO runtime: it requests the PICO hand-tracking extensions, synthesises FB aim/mesh data and writes the hand-passthrough hole. The hooks change process memory; the APK and system libraries on disk remain unchanged.

For the component breakdown, hooks, capability detection and initialization behavior, see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Repository layout

| Path | Contents |
|---|---|
| `mod/native/` | VD payload `libvdhs.so`, Shell receiver `libvdhs_shell.so`, tracking-capability policy and shared mapped-symbol resolver; vendored `third_party/shadowhook-2.0.1/` |
| `mod/src/VdHsMod/` | Managed mod (netstandard2.0, reflection only, no reference to Virtual Desktop assemblies) |
| `zygisk/` | Magisk/Zygisk module: injector `jni/`, `payload/` (`mode.txt`, `backend.txt`), `build.ps1` |
| `coldstart/` | VD-only system package capability helper and its build script |
| `tools/mesh/` | Converts the headset's own PICO hand mesh into `hand_mesh_fb.bin` |
| `assets/mesh/` | The hand mesh blob shipped with releases, with provenance and disclaimer |
| `config/` | `hand_gesture.txt` example with every runtime switch |
| `docs/` | Architecture notes |

## Building

Point `ANDROID_SDK_ROOT` (or `ANDROID_HOME`) at an Android SDK with NDK 26.1.10909125, CMake 3.22.1, platform 35 and build-tools 35.0.0, or write the SDK path into the untracked `tools/sdk-path.txt`. Set `JAVA_HOME` to JDK 17, or put its path in the untracked `tools/jdk-path.txt`. Then:

```powershell
# native payload + managed mod + Zygisk injector, staged into zygisk/payload/
pwsh -File zygisk/build.ps1

# additionally pack zygisk/dist/vdhs_zygisk.zip (the Magisk module)
pwsh -File zygisk/build.ps1 -Pack
```

The native payload must build with zero warnings; the default managed build restores no NuGet packages and compiles offline. Install the resulting `zygisk/dist/vdhs_zygisk.zip` through Magisk as described under [Manual installation](#manual-installation).

## Installation

### One-shot installer (recommended)

Download `VD-PicoEnhance-v<version>.zip` from [Releases](https://github.com/WolalaQAQ/VD-PicoEnhance/releases/latest) and extract it on the PC. The installer, module ZIP and hand mesh will be in the same directory. Open PowerShell there and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1
```

The script finds the headset over adb, checks root, installs the local module ZIP, places the mesh if missing, and reboots the headset. Existing meshes are preserved unless you explicitly pass `-MeshPath`. It looks beside the script regardless of the terminal's working directory, never downloads files, and stops if an input is missing. Enable USB debugging and authorize the PC. Install Android platform-tools (`adb`); the script checks PATH and the Android SDK environment variables, or you can supply `-Adb`.

Useful parameters: `-Mode 1` for an injection-only control run, `-NoReboot` to skip the automatic reboot, `-ZipPath` / `-MeshPath` to specify local files elsewhere, and `-Serial` to pick a device when several are connected. To install a different version, use the bundle from that release.

### Manual installation

1. If using a release bundle, extract it first. In Magisk choose **Modules → Install from storage**, select its `vdhs_zygisk.zip` or your locally built module ZIP, then reboot the headset.
2. Launch Virtual Desktop. The module installs `hand_mesh_fb.bin` if missing; no manual copy is needed.

`payload/mode.txt` defaults to `2`, the full layer. For an injection-only control run, set `/data/adb/modules/vdhs_zygisk/payload/mode.txt` to `1` as root and reboot the headset. This also unloads the Shell receiver and startup helper; restarting VD alone does not unload them.

To upgrade, run the installer from the new bundle to install the full module, then reboot the headset.

> On supported PICO firmware, full mode declares VD's hand-tracking capability so it can start while the system is in hand mode. See the startup-support section below; no APK modification is needed.

## Configuration

No configuration file is needed for the defaults. To adjust a feature, copy the relevant entries from [`config/hand_gesture.example.txt`](config/hand_gesture.example.txt) into `/data/data/VirtualDesktop.Android/vdhs/hand_gesture.txt` on the headset. Every key is optional; a missing key keeps its built-in default. The example lists configurable keys and suggested values.

SteamVR hand-passthrough holes are enabled by default (`pt_hole=1`) and follow VD's VR hand-passthrough setting. UI gaze smoothing and head-pose compensation are enabled by default; explicit values in an existing configuration override the defaults. Restart VD after saving changes.

### Starting VD in system hand mode

On PICO 4 Pro / PICO OS 5.13.7, full mode automatically supplies the integer `handtracking=1` declaration for VD in the system's in-memory package metadata. This allows the existing system launch check to recognize VD as hand-compatible. The official APK, signature, other applications and global hand-tracking settings remain unchanged. The helper is restricted to the verified Android 10 firmware fingerprint; other firmware is left unchanged.

Reboot once after installing. As root, run `sh /data/adb/modules/vdhs_zygisk/coldstart.sh status` and `sh /data/adb/modules/vdhs_zygisk/coldstart.sh query`. The status should show `applied` or `native`, the value `int1`, and a successful `ok` result; the independent query should show `metadata-query int1 ok`. Then test launching VD in hand mode. The helper rechecks VD after package updates.

For troubleshooting, `coldstart.sh disable` restores this helper's declaration and saves the disabled choice; `enable` turns it back on. Existing explicit choices are retained on upgrade. Smoke-test mode `1` does not load the helper; reboot after switching modes or disabling the entire module. This compatibility declaration is separate from the Input-tab gesture-suppression checkbox: it does not suppress gestures or switch between Index controllers and hands.

### System-gesture suppression (experimental, off by default)

Use **Input → Suppress system gestures** in VD. On a Chinese system the label is **屏蔽系统手势**. This is the only gesture-suppression switch: it is always available in full mode, defaults to off and remembers your choice. Changes normally take effect within one to two seconds without restarting VD or the headset.

The saved state is stored automatically in VD's private `vdhs/system_gestures.txt`; no manual file editing is needed. Existing saved UI choices are kept. The former module-level `payload/system_gestures.txt` is no longer used; a missing or invalid saved UI value means off.

In full mode, a background mirror checks the saved UI state once a second and writes the Shell property only when the value changes. The receiver stays loaded so the checkbox can enable suppression at any time, but loading it does not enable suppression.

When enabled, a separate Shell payload attempts to apply PICO's own streaming-app gesture-disable rule to focused VD, suppressing recenter/dock gestures. Other packages retain the original path. It does not inject into the OpenXR runtime service or change the global hand-tracking switch.

Unchecking the UI restores system gestures and keeps the checkbox available. Reboot once after installing this revision to load the Shell receiver. Disabling the entire Magisk module or selecting smoke-test mode `1` also requires a reboot to unload the receiver.

### Chinese UI

Localization follows the headset's system language automatically: Chinese uses the Simplified Chinese table; other languages keep VD's original UI. No manual opt-in is needed. The language is read when VD starts, so restart VD after changing the system language.

Translations cover the control panel and SteamVR performance overlay. Computer/game/file names, paths, network names, accounts, clocks, telemetry, device names and units retain their original text. Fixed card captions such as "Disconnect" and "Local file" are translated separately.

To customize translations, edit `/data/adb/modules/vdhs_zygisk/payload/ui_zh.txt` as root, then restart VD. Each entry is `English<TAB>Chinese`; literal `\n` encodes a line break. Remove that module table and restart VD to disable localization.

### Hand mesh

`XR_FB_hand_tracking_mesh` needs `hand_mesh_fb.bin` next to the payload, in the app's private directory:

```text
/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin
```

Without this file, the module does not enable VD's hand-mesh feature. The directory already exists once the module has run.

**Use the bundled mesh (recommended).** No manual copy is normally needed, and existing meshes are preserved. To replace a mesh, pass its path with the installer's `-MeshPath` parameter or run on the PC:

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
- PICO's "gestures not supported" launch prompt can still appear if startup support is disabled or the firmware is outside its supported fingerprint. See [Starting VD in system hand mode](#starting-vd-in-system-hand-mode).
- A Virtual Desktop update can change internal names and break the layer until it is adapted.

## TODO

- [ ] **USB wired streaming**: Virtual Desktop 1.34.22 supports wired streaming over USB NCM, but PICO does not currently expose that link to apps, so the app's USB mode is unavailable there. The plan is to bring up a USB network interface on the headset and let the compatibility layer use it, validating the link and discovery on-device before deciding whether to ship it.
- [ ] **Controller battery level**: pass the PICO controller battery level through to SteamVR, so the remaining charge is visible in SteamVR and in games.

## Uninstall and rollback

- Injection-only control: set `/data/adb/modules/vdhs_zygisk/payload/mode.txt` to `1` as root and reboot the headset. Switching back to `2` also requires a reboot so all components use the same mode.
- Disable or remove the module in Magisk, then reboot. To disable it manually, run as root:

  ```sh
  touch /data/adb/modules/vdhs_zygisk/disable
  ```

- After disabling or removing the module, you can delete `/data/data/VirtualDesktop.Android/vdhs/` to clean up staged files. This also removes saved switches, `hand_gesture.txt` and custom meshes; back up any files you want to keep first.

## Privacy and security

- The module needs **root** and injects code into Virtual Desktop. Use it only on your own device and only with a build you made yourself or trust.
- The injector stages payloads into the target app's private directory; it does not change the APK, manifest, signature or package name.
- Full mode loads the Shell receiver even while suppression is off. On the supported firmware it also loads the VD-only metadata helper into `system_server`. Neither component injects into the OpenXR runtime service or changes the global hand-tracking setting.
- This repository bundles no Virtual Desktop, Xenko, Mono or PICO binaries or sources, with the single exception of the hand mesh blob included in the release bundle; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- When opening an issue, include the headset model, PICO OS version, Virtual Desktop version and sanitized log excerpts. Do not upload device identifiers or signing keys.

## Disclaimer

This is an unofficial, experimental community project. It does not contain, forward or bypass Virtual Desktop, PICO or their licenses, and it modifies no installed app. Using a root module against a third-party app is at your own risk: it can break Virtual Desktop or the device, and you are responsible for complying with the terms of the software you use. "Virtual Desktop" and "PICO" belong to their respective owners and are used here only to describe interoperability.

## Acknowledgements

- Thanks to [misaka18848](https://github.com/misaka18848) for the PICO 4 compatibility work in [PR #1](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/1), the runtime gesture-suppression switch and Chinese localization in [PR #2](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/2), and testing the integrated changes on a PICO 4 headset.
- [ShadowHook](https://github.com/bytedance/android-inline-hook): the inline-hook runtime.
- John "topjohnwu" Wu: the Magisk / Zygisk module API.
- `hand_mesh_fb.bin` is derived from the PICO system hand mesh and converted with the offline tooling in `tools/mesh/`.

## License

[MIT](LICENSE). Third-party components remain under their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
