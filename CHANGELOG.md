# Changelog

English | [简体中文](CHANGELOG_zh.md)

## [Unreleased] - 2026-10-19

### Features

- Review the Chinese dictionary: preserve brand and technology names and humorous quality preset names, clarify passthrough windows, controller inputs, and frame-rate units, and distinguish feature-enablement limits from output quality.
- Localize the control panel's cached page and templates at runtime. Use text-change events for changing captions, and reinitialize after graphics-device content recreation.
- Control system-gesture suppression only through VD's Input-tab checkbox: "Suppress system gestures" / "屏蔽系统手势". Remove the separate module-level gesture configuration; default to off, retain saved UI choices and preserve the Shell's original focus condition.
- Enable Chinese automatically for the headset's Chinese system language; leave other languages unchanged. Translate UI-thread caption changes synchronously instead of queueing quality changes for a later frame.
- Localize the separate performance overlay and redraw its own target, retaining telemetry, units, device/GPU/codec names, text sizes and warning colors.
- Match saved system-gesture parsing to the background mirror so the displayed state agrees; include fixed network warnings on computer cards created before localization.
- Restore the original font when a mixed status field returns to a number, an untranslated technology name or a preserved quality-preset name.
- Avoid font reassignment and additional full-panel redraws for repeated UI-thread captions with the same translated appearance. Keep full redraws for actual changes and asynchronous updates that may have exposed English.
- Run the system-gesture receiver and state mirror in full mode; missing or invalid UI state is off, and property writes occur only when the normalized value changes.

### Design Rationale

- Reuse VD's cached UI resources, dispatcher, font and redraw path instead of recurring label scans or visual masking. Keep user data and live values separate from fixed captions.
- Read the same installed-system language API used by VD, not the game thread's invariant culture. The performance overlay has its own render target, so redrawing the settings page alone is insufficient.
- Keep receiver loading separate from the user's ON/OFF choice so enabling the checkbox needs no preliminary file edit or reboot. This requires a one-second state mirror in full mode even while suppression is off.

### Notes & Caveats

- Source/build candidate for VD 1.34.22.0; headset font, layout and interaction checks are pending. The official APK remains unchanged.
- Edit `payload/ui_zh.txt` and restart VD to change translations; literal `\n` represents a line break. Removing the table disables localization on the next VD launch.
- Chinese system locales use the Simplified Chinese table; restart VD after changing the system language. Visual checks and the complete system-gesture round trip remain pending.
- Reboot once after installing the single-switch revision. The former module-level `payload/system_gestures.txt` is ignored; existing app-private UI choices are preserved. Without a saved UI choice, suppression remains off.

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
