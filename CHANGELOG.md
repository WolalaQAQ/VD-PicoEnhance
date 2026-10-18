# Changelog

English | [简体中文](CHANGELOG_zh.md)

## 1.0.9 — 2026-10-18 — system hand-gesture suppression

- **System hand gestures are suppressed while Virtual Desktop runs.** The recenter / open-dock gestures are performed by the system shell, not by the app, so no layer inside Virtual Desktop can stop them. Reverse engineering the shell (`com.picoxr.xrshell`) showed how PICO's own streaming app avoids them: the shell keeps a hardcoded list of packages it refuses to act for — `gDisableSystemGesturesPackageNames`, built in a static initialiser and containing exactly one entry, `com.picovr.picostreamassistant` — and `InputManager::updateAppDisableSystemGestures(pkg, displayState, isSelf)` re-evaluates it every frame to drive `gApplicationDisableSystemGestures`. A third Zygisk target (`com.picoxr.xrshell`, payload `libvdhs_shell.so`) hooks that function and answers the per-frame check for `VirtualDesktop.Android` the same way, so the shell stops acting on gestures while VD is focused and behaves normally again as soon as it is not.
- **Runtime-side companion payload.** `libvdhs_rt.so` is injected into PICO's out-of-process OpenXR runtime (`com.pico.xr.openxr_runtime`) and clears the hand-gesture state the runtime relays to every session (the shell included) while VD owns the MR service (`sys.pxr.mrserivce.user`). It hooks `InputDeviceManager::service_status_callback`, and — once that library is mapped — the runtime's own gesture emitter, located through the log string it prints, dropping the event before it is delivered.
- **Shared symbol resolver.** The in-memory ELF resolver moved to `vdhs_sym.c` / `vdhs_sym.h` and gained a fallback for APK-embedded libraries: PICO's runtime libraries are mapped straight out of `XRRuntime.apk`, so nothing in `/proc/self/maps` names them. Candidate bases are taken from the ELF headers those mappings begin with, and every read is bounds-checked against the image's own segments, so a stale or truncated mapping can no longer fault the target process.
- Verified on the headset: gestures no longer fire in VD, return immediately after leaving it, and hand tracking plus hand passthrough are unaffected.

## 1.0.8 — 2026-10-18 — PICO 4 (A8110) port

- **PICO 4 support**: remove eye-gaze and face-tracking from the native layer and the managed mod, since the device has neither. `vdhs_layer.c` drops the gaze block (`hk_create_action_space`/`hk_locate_space`, the Mono stack walker, the One-Euro filter), the gaze hook entries, the `eye_gaze_pose` branch in `hk_create_action`, and the gaze permission fix; `vdhs_hand.c` drops the gaze config fields and the `eye_gaze` restore path. `module.prop` / `customize.sh` label the build "PICO 4 slim: no eye/face tracking".
- **Active-input fallback**: the PICO 4 runtime exports no active-input-device query (every spelling returns `XR_ERROR_FUNCTION_UNSUPPORTED`). The layer now reads `pvr.active.input_device` (`2` = hands) in `vdhs_hand_active_input()` and masks the grip pose's `isActive`, which is what makes VD switch to hands. Verified on VD 1.34.18.0 (`exts 16->19`, `supportsHandTracking=1`, `hand_ext=1`, `locate ... isActive=1`, VD pulls the hand mesh for both hands). SteamVR hands and hand passthrough work.
- **Packaging**: stage a Mono-compatible `0Harmony.dll` (`netstandard2.0` / `net472`) instead of `net35`, which fails to load on the headset's Mono (`mscorlib, Version=2.0.0.0`). Pack the ZIP with .NET `ZipArchive` and forward slashes (PowerShell 5.1's `Compress-Archive` writes backslashes).
- The managed `IsControllerActive` switch (Harmony/redirect) is **not** required on PICO 4; `auto` → `dataonly` is enough.

## 1.0.7 — 2026-10-10

- Balance Mono GC thread-state transitions around gaze-consumer stack walks, correcting the `STATE_BLOCKING / DO_BLOCKING` abort path when the first gaze queries race on entering SteamVR. Skip pointer enhancements if the transition APIs are unavailable; streaming gaze retains upstream results.
- On 2026-10-10, two app launches followed by SteamVR entry passed on PICO 4 Pro / VD 1.34.22.0 with PicoET dual/on. The user confirmed no crash and manually exited both runs. One run logged HMD-only input; both completed their first concurrent gaze classifications, with no new crash-buffer entries after installation. Long-duration stability has not yet been assessed.

## 1.0.6 — 2026-10-08

- Enable all implemented user-facing features by default, including SteamVR hand-passthrough holes (`pt_hole=1`) and the 300 ms joint freeze gate. Default the hole radius to 0.1 m to match VD's hand window.
- Bundle the mesh in the Magisk ZIP and let Zygisk install it when missing on first launch; manual module installation no longer needs a mesh-copy step. Preserve custom meshes unless an explicit installer `-MeshPath` replaces one.
- Keep explicit `hand_gesture.txt` settings. SteamVR hand passthrough still follows VD's own switch without forcing app settings. Debug logging, alpha-only diagnostics and the `pt_style` A/B diagnostic default off.
- Managed and native builds complete without warnings; module packaging, shell/PowerShell syntax checks and public-content checks pass.
- These defaults and automatic mesh installation have not yet been validated on a headset.

## 1.0.5 — 2026-10-03

- GitHub Actions builds, packages and publishes the complete installer bundle when a version tag is pushed. Each release now publishes only `VD-PicoEnhance-v<version>.zip`; the installer, module ZIP, mesh, license notices and checksums are all inside it.
- Update the installer help and documentation to use the bundle download.

## 1.0.4 — 2026-10-03

### Fixed

- **Foveated streaming**: restrict the module's gaze smoothing, jump confirmation, invalid-sample hold and head-pose compensation to UI-pointer calls. Streaming, tracking forwarding and unknown callers retain upstream poses and validity flags. Filter history is now thread-local.
- **Startup heap overflow**: reserve space for both added hand and controller extensions.
- **First-install permissions**: set the app payload directory's owner, permissions and SELinux labels so the app can write its payload.
- **Partial hook installation**: activate core and passthrough hook groups only when each group is complete, and roll back failures. Inactive callbacks pass through. The managed loader and input probe no longer attempt runtime negotiation after hook installation fails.
- **Hand rays**: accept a runtime ray only when both computed and valid flags are set and its pose values are valid; otherwise use the synthesised ray instead of a stale pose.
- **Pinch configuration**: correct example thresholds to `0.4/0.8/3.0` cm and clarify the strength formula. Validate finite values and `0 ≤ on < off ≤ full`, restoring the whole group to defaults when invalid.
- **Installer failures**: check every ADB exit code and stop after remote-command failures, avoiding stale files left by failed transfers.
- **Stale payloads**: remove unused redirect build artifacts and staged optional DLLs or configuration files no longer supplied by the module. Write payloads through temporary files and atomic replacement; skip injection if a required file is missing or copying fails.

### Changed

- Enable UI-pointer head-pose compensation by default (`gaze_vd_fix=1`). Gaze smoothing remains enabled by default. Both settings apply only to the pointer.
- Provide separate English and Chinese changelogs linked from the READMEs, and correct the manual mesh-installation permissions.
- Add `VD-PicoEnhance-v1.0.4.zip`, bundling the installer, module ZIP, hand mesh, license notices and checksums in one download.

### Upgrading

Install the full module and reboot the headset: this update includes the Zygisk injector. The installer preserves `hand_gesture.txt`; an explicit `gaze_vd_fix=0` continues to disable compensation. Change it to `1` or remove the line to use the new default.

If staying on v1.0.3, set both `gaze_vd_fix` and `gaze_filter` to `0` when streaming, then restart VD. That release applies pointer processing to streaming gaze too.

### On-device observation

The fixes passed one functional observation on PICO 4 Pro / VD 1.34.22.0 on 2026-10-02: hand/controller switching, pinch and ray recovery, both-hand passthrough, gaze and app restart all worked normally. Head-pose compensation was enabled (`gaze_vd_fix=1`), with gaze smoothing at its default of `1`. Logs confirmed pointer processing and streaming bypass; the captured crash buffer was empty.

## 1.0.3 — 2026-10-02

- The installer now uses the local ZIP and mesh beside the script, stops when files are missing, and no longer downloads assets automatically.

## 1.0.2 — 2026-10-02

- Add the one-shot installer as a release asset, covering module installation, hand mesh placement and headset reboot.

## 1.0.1 — 2026-10-02

- Enable the full compatibility layer by default (`mode=2`) and document hand mesh installation.

## 1.0.0 — 2026-10-02

- Initial public release with PICO hand tracking, hand/controller switching, hand passthrough, gaze-pointer processing, and build/release workflows.
