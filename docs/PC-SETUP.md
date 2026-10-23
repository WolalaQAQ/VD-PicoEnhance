# Installation and use

English | [简体中文](PC-SETUP_zh.md)

The release bundle installs two things. The base is VD-PicoEnhance with PICO controller battery in SteamVR, and it is always installed. **PicoHand<->Knuckles** is an optional module that switches automatically between Valve Index controllers (Knuckles) and PICO optical hand tracking; on a first installation it is added only when you ask for it with `-WithKnuckles`, and once installed, upgrades keep it.

## Before you install

The base needs:

- a rooted PICO 4 or PICO 4 Pro with Zygisk enabled and the official Android VD 1.34.22.0;
- a Windows PC with the supported VD Streamer 1.34.22.0 and SteamVR;
- Android platform-tools (`adb`) on the PC, with debugging and the shell's root request authorized on the headset.

PicoHand<->Knuckles also needs Valve Index controllers in SteamVR, a PICO 4 Pro on PICO OS 5.13.7 (the firmware its headset backend currently supports; other firmware is refused), and the PC and headset on the same private local network (10.x, 172.16–31.x or 192.168.x addresses) with a path between them.

## Install once

1. Download and fully extract the release bundle. Exit VD and SteamVR, then connect the headset over USB or authorized wireless ADB.
2. Open a normal PowerShell under the Windows account that runs SteamVR and run from the extracted folder:

   ```powershell
   # base only
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1

   # base plus PicoHand<->Knuckles
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -WithKnuckles
   ```

   Add `-Adb 'C:\path\to\adb.exe'` if adb is not on PATH, `-Serial <target>` when several devices are connected, or `-DriverPath <complete driver DLL path>` for a nonstandard Streamer location. Approve the Windows administrator prompt for the PC file operations; the whole terminal need not be elevated.
3. The installer patches the PC copy of VD's driver so it publishes controller battery, installs the headset module and the missing hand mesh, and reboots the headset once. With `-NoReboot` you reboot it yourself, and the module takes effect on the next boot.

With `-WithKnuckles`, the installer also installs PicoHand<->Knuckles: its own SteamVR driver on the PC, its own daemon on the headset, and a one-time pairing between them. It reads the headset's Wi-Fi address and the PC address facing it automatically; if it picks the wrong adapter, add `-HeadsetAddress <address> -PcAddress <address>`. The headset reboot starts the daemon. The flag needs a full installation, so the installer rejects it together with `-HeadsetOnly` or `-Mode 1` instead of skipping it. On a first installation without the flag, none of the module is installed and the headset's hand-tracking setting is left alone; a module that is already installed is kept and upgraded.

## Daily use

Start VD and SteamVR as usual. Battery display needs nothing else.

PicoHand<->Knuckles starts with SteamVR. Its driver connects to the headset daemon over the local network, needs no ADB and no frames from VD, adds no virtual device or resident program on the PC, and works whether or not VD is running. Start SteamVR first and switch on the Index controllers afterward, which is the order it was tested in.

With automatic mode running (the default, with no separate start step), once the controllers have connected and track steadily, **touch and fully release the trackpad of each powered-on controller once**, including one you plan to leave unused. Until a connected controller has reported its touch state, the module cannot tell that it is released, and that blocks switching back to hands.

- **Pick up:** touch the trackpad of an Index controller while it is moving. Touch and motion must come from the same controller. The module then turns off the system's hand tracking, including its hand gestures. Hands stop appearing, and the system menu and recentering gestures do not trigger while you use the controllers. VD's own **Suppress system gestures** switch is a separate setting and stays as you set it.
- **Put down:** release the trackpad on every present Index controller and keep them still for one second. System hand tracking comes back and your hands return. One controller on its own works the same way, and so does a controller still in your hand: if the thumb leaves the trackpad and the controller stays still, hand tracking returns. To stay in Index mode, keep a thumb on the trackpad.
- **No Index controllers:** the module idles. It only asks the headset for its status, writes no hand-tracking preference and changes no system setting, so PICO controllers and optical hands behave as usual.
- **Lost signal:** a controller that is connected but has unknown trackpad state, or has lost tracking, does not count as released. If every Index controller disappears, the module restores the original state and returns to standby.

When control stops, for example with Stop or when SteamVR closes, the headset's original hand-tracking state is restored. If the PC disappears suddenly, the 3-second control lease on the headset runs out and triggers the same restore.

Tested on PICO 4 Pro / PICO OS 5.13.7: switching in both directions with both controllers, no system menu or recentering in Index mode, and hand tracking and the PICO menu coming back after putting the controllers down.

### How the PC finds the headset

After pairing, the driver finds the headset with a short UDP request, sent to the saved address and to multicast group 239.255.255.250 on the PC's active private Ethernet and Wi-Fi interfaces. The headset answers, and the PC opens the TCP control connection to the address the answer came from. Discovery and control both use port 29744. The face-tracking Bridge (port 9030) is not involved and does not need to be running.

An address change on the same LAN, on either device, therefore needs no new pairing. The headset checks its Wi-Fi address every second; when it changes, the headset first restores the original hand-tracking state and then listens on the new address. After a network failure, automatic mode restores the original state and retries after a pause of about two seconds, searching again and opening a fresh session. When SteamVR returns from standby, the driver can discover the headset and open a new session again. A saved Stop or Disable stays stopped, and an error reported by the headset, a protocol error or an unfinished restore is not retried automatically.

Your network must pass UDP multicast and allow TCP and UDP 29744 between the two devices; guest networks with client isolation can block this. When no headset answers, the PC tries the saved address, which also works with a headset that still runs the older fixed-address build.

## Disable, enable and status

Run these from the extracted folder, with the headset connected for Disable and Enable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Disable
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Enable
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Status
```

Disable and Enable apply only to PicoHand<->Knuckles. Disable restores the headset's hand tracking, stops switching and keeps the choice across reboots and SteamVR restarts; Enable turns it back on. Neither affects the base module or battery display.

Status reads the saved configuration and shows whether PicoHand<->Knuckles is installed and enabled, alongside the state of the PC battery patch. It does not contact the headset or SteamVR, so it shows what is configured, not whether switching is working at that moment.

## Upgrade, pairing and removal

- **Upgrade:** exit VD and SteamVR, then run the same installer from the new release bundle; this includes upgrading from 1.0.8. The PC patch is rebuilt from the original backup and the headset module is upgraded with one reboot. If PicoHand<->Knuckles is installed, it is upgraded with them and keeps its enabled or disabled choice without repeating `-WithKnuckles`.
- **Refresh pairing:** if the saved addresses are out of date and discovery cannot make up for it, because the network blocks multicast or one side still runs the older fixed-address version, exit SteamVR, connect the headset and run:

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Configure
  ```

  Add `-HeadsetAddress <address> -PcAddress <address>` to set the addresses yourself. Configure re-pairs the two sides and keeps your enabled or disabled choice. When only the ADB address changes, pass `-Adb <path> -Serial <new endpoint>` to the command you are running.
- **Remove only PicoHand<->Knuckles:** exit SteamVR, connect the headset and run:

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action UninstallKnuckles
  ```

  This restores the headset's hand tracking, unregisters the SteamVR driver and marks the headset module for removal, which takes effect at the next headset reboot. VD-PicoEnhance and battery display stay as they are. To install it again, run the installer with `-WithKnuckles`.
- **Remove everything:** exit VD and SteamVR, connect the headset and run:

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\install-vd-picoenhance.ps1 -Action Uninstall
  ```

  If PicoHand<->Knuckles is installed, it is stopped, restored and unregistered first. Then the original PC driver is restored, the headset module is marked for removal and the headset reboots. `-NoReboot` leaves rebooting to you; removal takes effect on the next boot. The original driver backup and the pairing are kept.

## What the installer changes

The PC patch changes only Streamer's `OpenVRDriver/bin/win64/driver_VirtualDesktop.dll`, and it keeps the exact original and a receipt beside it. It carries battery display only; PicoHand<->Knuckles is a separate driver. Unsupported versions, unrelated modifications and changes while SteamVR runs are refused, and a later Streamer update is not overwritten with an old backup. The patched driver is generated on your PC and is unsigned; no Windows security-policy change is needed. The official Android APK stays unchanged.

PicoHand<->Knuckles adds its own SteamVR driver and registers it with SteamVR. Valve's Lighthouse driver, the controllers' firmware, their bindings and calibration are left as they are. The pairing key is stored for your Windows user and SYSTEM only.

The release bundle contains no Virtual Desktop DLL, original or patched; the installer patches the copy already on your PC.
