# Changelog

English | [简体中文](CHANGELOG_zh.md)

## 1.0.9 — 2026-10-24

This version shows PICO controller battery in SteamVR and adds **PicoHand<->Knuckles**, an optional module that switches between Valve Index controllers (Knuckles) and PICO optical hand tracking as you pick the controllers up and put them down. One installer sets up both.

### Features

- **Controller battery in SteamVR.** PICO controller battery levels travel in VD's existing controller data and reach SteamVR through a patch applied to your own copy of VD's PC driver. The patch carries battery only. When a controller temporarily loses its reading, the battery is marked unavailable instead of being reported as 0%; a real 0% still shows.
- **PicoHand<->Knuckles (optional).** Pick up an Index controller and the system's hand tracking turns off, including its hand gestures; put it down and your hands come back. A pickup is a trackpad touch plus motion on the same controller. A putdown is every present controller released, with one second of stillness. The PC decides when that happens, and an independent root daemon on the headset applies it over your local network. Switching does not rely on Virtual Desktop's video stream, on hooks inside VD, on the face-tracking Bridge or on ADB, and the PC side is a small SteamVR driver that starts and stops with SteamVR.
- **Automatic discovery and reconnection.** After a one-time pairing, the PC driver finds the headset on the local network, keeps finding it when either address changes within the same LAN, and reconnects after a short network drop or SteamVR standby. A saved Stop or Disable stays stopped.
- **Newer SteamVR.** Trackpad input is also read through SteamVR's DriverInput 005 interface; 003 and 004 support is kept for older versions.
- **One installer.** `install-vd-picoenhance.ps1` installs VD-PicoEnhance and battery display by default, and on a first installation adds PicoHand<->Knuckles with its pairing only when you pass `-WithKnuckles`. The same script offers `-Action Disable`, `Enable`, `Status`, `Configure` (refresh the pairing), `UninstallKnuckles` and `Uninstall`. An already installed module is kept and upgraded, with its enabled or disabled choice, whether or not the flag is repeated.
- **Steadier boot and upgrades.** The headset module now does its own startup work only after Android reports that boot is complete. Upgrades from 1.0.8 are covered, and any earlier test build of the Knuckles switcher is retired in order before the new service starts.
- **Hand-priority restore.** The PC driver sets the optical hands' priority only after a real switching decision, and puts the original priority back on Stop. If that restore has not finished, the original value stays saved, and a last attempt runs before SteamVR unloads the driver.

### Install and upgrade

Download `VD-PicoEnhance-v1.0.9.zip`; it holds the installer, the module packages and the hand mesh, so nothing else is needed. Extract it, exit VD and SteamVR, connect the headset with debugging and root authorized, and run from the extracted folder:

```powershell
# VD-PicoEnhance and PICO controller battery
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1

# the same, plus PicoHand<->Knuckles
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -WithKnuckles
```

The headset reboots once at the end. After that, start VD and SteamVR as usual. To upgrade from 1.0.8, run the same command from this bundle; an installed PicoHand<->Knuckles is upgraded along with it. [Installation and use](https://github.com/WolalaQAQ/VD-PicoEnhance/blob/v1.0.9/docs/PC-SETUP.md) covers daily use, disabling and removal.

### Design Rationale

- PicoHand<->Knuckles is its own SteamVR driver and its own headset daemon, connected directly over the local network. Keeping it apart from Virtual Desktop leaves the PC patch with battery only.
- It is opt-in. The base installation never changes the headset's hand-tracking setting; only people who choose the module get a root daemon that does.
- The daemon reuses the headset's existing system hand-tracking backend. It saves the original setting before changing it, and restores it on Stop, a lost connection, a three-second control lease running out, or a crash.
- Without Index controllers the module only asks the headset for its status. It writes no hand-tracking preference and no system setting. A missing reading is never treated as a release, and when every controller disappears the original state is restored.
- Discovery is a small UDP exchange that stays separate from the existing TCP control protocol. It reuses the same pairing key, and the TCP protocol itself is unchanged. It adds no program on the PC and does not depend on the face-tracking Bridge.
- The release contains no Virtual Desktop DLL, original or patched. The battery patch is generated on your PC from your own copy of the driver, and the installer keeps the exact original.
- Early boot on PICO OS 5.13.7 has a platform stall, also seen with the modules disabled, that can delay audio and the desktop by a minute or more. Running the module's own work after boot completes keeps it out of that window. VD's OpenXR layer still loads when VD starts, because it must be in place before VD enumerates extensions.

### Notes & Caveats

- PicoHand<->Knuckles is for both PICO 4 and PICO 4 Pro and does not require eye- or face-tracking hardware. For both models, its headset backend currently requires an exact firmware fingerprint match to the verified PICO OS 5.13.7 build. It needs root on the headset, Index controllers in SteamVR, and the PC and headset on the same private local network, where UDP multicast and TCP/UDP port 29744 can pass; guest networks with client isolation can block it.
- With automatic mode running and controller tracking steady, touch and fully release each powered-on controller's trackpad once, including one you are not using. A connected controller whose touch state is unknown does not count as released.
- Discovery and control send the pairing key in plaintext, so use them on a trusted home network and do not forward port 29744 to the internet. Pairing is stored for the Windows user who ran the installer.
- Exit VD and SteamVR before running the installer. ADB is used only for installation, pairing and persistent headset management.
- The PC battery patch targets VD Streamer 1.34.22.0 for Windows x64; unknown versions and outside modifications are refused. If battery data is unavailable, controller input carries on as before.
- Checked on PICO 4 Pro / PICO OS 5.13.7 with VD 1.34.22.0: upgrading an official 1.0.8 module with this bundle and booting afterwards, with the existing Knuckles module, battery patch and pairing kept; pickup and putdown with both controllers in both directions on a current SteamVR; automatic discovery, reconnection after SteamVR standby, and Stop. Earlier tests of the same switching logic showed no system menu or recentering in Index mode and hands returning after putdown. A real router DHCP address change was not tried; address following is covered by simulated network tests.

## 1.0.8 — 2026-10-19

### Features

- Support ordinary PICO 4 through tracking-capability detection while retaining the Pro eye-tracking path.
- Add one VD Input-tab switch for system-gesture suppression, off by default with the user's choice saved. No external enablement setting is needed.
- Localize the control panel and SteamVR performance overlay automatically on Chinese systems; keep names, telemetry and units unchanged.
- Fix changing captions and mixed-font fields, and avoid extra redraws when the translated appearance has not changed.
- Enable hand-mode VD startup on the verified PICO 4 Pro / OS 5.13.7 Android-10 firmware fingerprint, without changing the official APK or global tracking settings.
- Share mapped-library symbol resolution through xDL and produce portable module ZIPs on PowerShell 5.1 and 7.

### Design Rationale

- Gate tracking changes by capability rather than removing Pro features to support PICO 4.
- Reuse VD's UI resources and text-change events. Keep the in-memory launch declaration separate from the Shell's gesture-suppression switch.

### Notes & Caveats

- Install the full module and reboot the headset. Existing feature settings, custom meshes and saved UI choices are preserved; the former module-level `payload/system_gestures.txt` is ignored. Changing payload mode also requires a headset reboot.
- Tested with VD 1.34.22.0 on PICO 4 Pro / OS 5.13.7; the contributor confirmed ordinary PICO 4 works. All 104 regression tests, the build and PowerShell 5.1/7 packaging checks passed with no build warnings or errors.
- Startup support retains explicit enable/disable choices. Build requirements include JDK 17, Android platform 35 and build-tools 35.0.0.
- Thanks to [misaka18848](https://github.com/misaka18848) for the PICO 4 work in [PR #1](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/1), the UI switch and localization in [PR #2](https://github.com/WolalaQAQ/VD-PicoEnhance/pull/2), and on-device testing.

## 1.0.8-dev1 — 2026-10-18

### Features

- Detect tracking capabilities instead of deleting the Pro eye-gaze path for headsets without the corresponding hardware. Keep pointer-only gaze enhancements and unmodified streaming gaze.
- Add opt-in PICO Shell gesture suppression, adapted from PR #1. `payload/system_gestures.txt` defaults to `0`; full mode and a headset reboot are required to enable it.
- Centralize mapped-object symbol resolution on the already-vendored xDL, keeping each symbol batch bound to one library without force-loading or cross-APK fallback.
- Write module ZIPs with explicit forward-slash entry names on Windows PowerShell 5.1 and PowerShell 7, with offline archive-content checks.

### Design Rationale

- Keep capability detection separate from input-selection policy. Retain the application-level active-input query; do not add a property fallback, a stationary-controller timeout, or Harmony changes.
- Apply gesture suppression only at the Shell's per-app decision. Do not add the runtime-service event interceptor, global hand-tracking changes, or process-importance heuristics.

### Notes & Caveats

- This is a source/build candidate, not a published release. It has not been installed or tested on a headset; new device support and Shell behavior still require device validation.
- Disable gesture suppression or select mode `1`, then reboot to remove an already-installed Shell hook. Restarting VD alone does not unload it.

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
