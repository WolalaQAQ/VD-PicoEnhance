# Changelog

English | [简体中文](CHANGELOG_zh.md)

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
