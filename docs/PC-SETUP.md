# PC Setup & Daily Use

**English** | [简体中文](PC-SETUP_zh.md)

The release bundle installs two parts:

- **Base (always installed)**: VD-PicoEnhance headset module + local PC driver patch for PICO controller battery in SteamVR.
- **PicoHand<->Knuckles (optional)**: Automatic switching between Valve Index controllers (Knuckles) and PICO optical hand tracking. First-time installs add it only when you pass `-WithKnuckles`; once installed, upgrades keep and update it automatically.

## Prerequisites

**Base install:**
- Rooted **PICO 4** or **PICO 4 Pro** with Zygisk enabled and unmodified Android Virtual Desktop **1.34.22.0** or **1.34.25.0**.
- Windows x64 PC with Virtual Desktop Streamer **1.34.22.0** or **1.34.25.0** and SteamVR.
- Android platform-tools (`adb`) on the PC, with USB/wireless debugging and shell root authorized on the headset.

**PicoHand<->Knuckles (additional requirements):**
- Valve Index controllers paired in SteamVR.
- PC and headset reachable on the same private LAN (`10.x`, `172.16–31.x`, or `192.168.x`). Works on both PICO 4 and PICO 4 Pro without eye- or face-tracking hardware.
- Exact firmware fingerprint match on the headset:

```sh
adb shell getprop ro.build.fingerprint
# Required:
# Pico/Phoenix/PICOA8110:10/5.13.7/smartcm.1761755159:user/dev-keys
```

> [!IMPORTANT]
> The headset daemon enforces an exact fingerprint check. A different build fingerprint is refused even if its version number is `5.13.7`.

## Installation

1. Download and extract the full `VD-PicoEnhance-v<version>.zip` release archive on your PC.
2. Exit **Virtual Desktop** and **SteamVR**, then connect the headset over USB or authorized wireless ADB.
3. Open a regular PowerShell window under the Windows account that runs SteamVR, change into the extracted folder, and run:

```powershell
# Base only (VD-PicoEnhance + controller battery)
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1

# Base + PicoHand<->Knuckles
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -WithKnuckles
```

Approve the Windows administrator prompt when requested for PC file operations; the terminal itself does not need to be elevated.

The installer patches the local PC copy of VD's driver, installs the Magisk module and missing hand mesh, pairs PicoHand<->Knuckles if requested, and reboots the headset once.

### Common Installer Options

| Parameter | Purpose |
|---|---|
| `-WithKnuckles` | Install and pair PicoHand<->Knuckles (requires full install; cannot combine with `-HeadsetOnly` or `-Mode 1`) |
| `-Adb <path>` | Path to `adb.exe` if not on `PATH` or `ANDROID_HOME` / `ANDROID_SDK_ROOT` |
| `-Serial <id>` | Target device serial or wireless ADB endpoint when multiple devices are connected |
| `-DriverPath <dll>` | Full path to `driver_VirtualDesktop.dll` if Streamer is in a nonstandard directory |
| `-HeadsetAddress <ip>` / `-PcAddress <ip>` | Override auto-detected private LAN addresses during Knuckles pairing |
| `-NoReboot` | Skip the automatic headset reboot at the end (changes apply on next boot) |
| `-MeshPath <file>` | Replace an existing `hand_mesh_fb.bin` instead of preserving it |
| `-HeadsetOnly` | Install only the headset Magisk module without modifying the PC |
| `-Mode 1` | Set injection-only smoke-test mode instead of full mode (`2`) |

## Daily Use

Start Virtual Desktop and SteamVR normally. PICO controller battery appears in SteamVR automatically with no extra steps.

**PicoHand<->Knuckles** runs inside SteamVR as a lightweight driver (`driver_picomixedvr`) with no virtual devices, no resident helper app, no ADB requirement, and no dependency on Virtual Desktop being open. Start SteamVR first, then power on your Index controllers.

> [!IMPORTANT]
> **First touch after powering on controllers**: Once each Index controller connects and tracks steadily, **touch and completely release its trackpad once**—including a controller you plan to leave sitting unused. Until a powered-on controller reports its touch state at least once, the switcher treats it as unknown and will not switch back to hand tracking.

### How Automatic Switching Works

- **Pick up**: Touch an Index controller's trackpad while moving that same controller. System hand tracking turns off immediately, preventing stray hands and accidental PICO system menu or recenter gestures. VD's own **Suppress system gestures** checkbox stays independent.
- **Put down**: Release the trackpad on every connected Index controller and keep them stationary for **1 second**. System hand tracking and the PICO menu gesture return automatically. (If you still hold a controller with your thumb off the trackpad and keep it still, hands return; rest a thumb on the trackpad to stay in Index mode.)
- **No Index controllers**: The driver stays in standby, reads headset status only, and changes no system settings. PICO controllers and hand tracking behave normally.
- **Lost tracking or disconnect**: A controller with unknown touch state or lost tracking does not count as released. If all Index controllers disappear, SteamVR exits, or the PC drops off the network, the headset restores its original hand-tracking state—backed by a **3-second control lease** on the daemon.

### LAN Discovery & IP Changes

After initial pairing, the PC driver locates the headset with a short UDP discovery packet sent to both the saved address and multicast group `239.255.255.250` on active private Ethernet/Wi-Fi adapters, then opens a TCP control session to the replying address. Discovery and control both use **port 29744** (separate from Face-Tracking Bridge on port `9030`).

- **DHCP IP changes**: Changing IP addresses on the same LAN requires no re-pairing. The headset daemon checks `wlan0` every second; if its address changes, it restores hand tracking first and rebinds to the new address.
- **Transient drops & standby**: After a temporary network drop or SteamVR standby, automatic mode restores state, pauses ~2 seconds, and reconnects on its own. Saved `Disable` states and explicit headset/protocol errors stay stopped instead of looping.
- **Router requirement**: Allow UDP multicast and TCP/UDP `29744` between PC and headset. Guest Wi-Fi networks with client isolation can block discovery; if multicast is blocked, the driver falls back to the saved IP address.

## Management Commands

Run these from the extracted release folder. Connect the headset via ADB for every action except `Status`.

| Command | Effect |
|---|---|
| `.\install-vd-picoenhance.ps1 -Action Status` | Reads local PC config and shows battery patch status + whether PicoHand<->Knuckles is installed and enabled (offline; does not contact headset or SteamVR) |
| `.\install-vd-picoenhance.ps1 -Action Disable` | Pauses PicoHand<->Knuckles, restores headset hand tracking, and persists across reboots (base module & battery unaffected) |
| `.\install-vd-picoenhance.ps1 -Action Enable` | Re-enables PicoHand<->Knuckles |
| `.\install-vd-picoenhance.ps1 -Action Configure` | Re-pairs LAN credentials/addresses (add `-HeadsetAddress` / `-PcAddress` to set manually) |
| `.\install-vd-picoenhance.ps1 -Action UninstallKnuckles` | Restores headset hand tracking, unregisters `driver_picomixedvr`, and marks the Knuckles headset module for removal on next reboot |
| `.\install-vd-picoenhance.ps1 -Action Uninstall` | Full removal: removes Knuckles (if installed), restores the original PC VD driver, marks `vdhs_zygisk` for removal, and reboots |

Prefix any command with `powershell -NoProfile -ExecutionPolicy Bypass -File` when running from standard Windows PowerShell, and pass `-Adb` / `-Serial` if needed.

### Upgrading

Exit Virtual Desktop and SteamVR, then run `install-vd-picoenhance.ps1` from the new release bundle. The PC battery patch is rebuilt cleanly from the saved original backup, and the headset reboots once. If PicoHand<->Knuckles is already installed, it upgrades automatically while keeping your enabled/disabled choice and pairing.

## Troubleshooting

| Symptom | What to Check |
|---|---|
| Hands do not return after putting down Index controllers | Touch and fully release the trackpad on *every* powered-on Index controller once so none remain in an unknown touch state, then keep them still for 1 second. |
| Knuckles driver cannot find the headset | Verify PC and headset are on the same private LAN (no guest/AP client isolation) and Windows Firewall allows TCP/UDP `29744`. If multicast is blocked and IPs changed, run `-Action Configure`. |
| Installer refuses to patch PC driver | Exit SteamVR completely (`vrserver.exe` must not be running) and verify Virtual Desktop Streamer is version `1.34.22.0` or `1.34.25.0`. |
| Controller battery stops showing after a Streamer update | Streamer overwrote `driver_VirtualDesktop.dll`. Exit SteamVR and re-run the installer from a release matching the updated Streamer version. |

## What the Installer Modifies

- **VD PC Driver**: Patches only `OpenVRDriver/bin/win64/driver_VirtualDesktop.dll` inside Virtual Desktop Streamer for battery reporting, storing the exact original binary and a receipt beside it. Unsupported versions and external modifications are refused, and future Streamer updates are never overwritten with an old backup. The patched DLL is generated locally on your PC and unsigned; no Windows security-policy change is needed.
- **PicoHand<->Knuckles**: Registers its own independent SteamVR driver (`driver_picomixedvr`) and installs a separate Magisk module (`pico_mixedvr_standalone`) on the headset. Valve's Lighthouse driver, controller firmware, bindings, and calibration are untouched. Pairing keys on Windows are ACL-restricted to your user account and `SYSTEM`.
- **Android APK**: Never modified. No original or patched Virtual Desktop DLL is shipped in the release bundle.
