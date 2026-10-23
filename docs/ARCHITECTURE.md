# Architecture

VD-PicoEnhance adds PICO hand tracking, hand passthrough and eye-gaze fixes to
the **unmodified** Virtual Desktop app. It does not patch, repack, re-sign or
rename the APK. The compatibility layer runs inside Virtual Desktop. Full mode
also loads a gesture receiver inside PICO's Shell; suppression defaults to off.
A VD-only package-metadata helper supplies hand-mode startup support inside
system_server on the verified PICO firmware.

This document describes the current source's Zygisk route for readers who want
to understand or build on the code. Installation instructions are in the README.

## 1. Components

```
Magisk / Zygisk
   └─ zygisk/arm64-v8a.so            injector, matches VirtualDesktop.Android
         └─ stages payload/ into <app_data>/vdhs/ at pre-app-specialize
         └─ dlopen(libvdhs.so) -> vdhs_payload_main()
               ├─ libvdhs.so          native OpenXR compatibility layer
               │     ├─ vdhs_layer.c  inline hooks on VD's bundled libopenxr_loader.so
               │     ├─ vdhs_hand.c   hand tracking, aim/mesh, managed extension list
               │     ├─ vdhs_pt.c     zero-copy hand-passthrough hole
               │     ├─ vdhs_mark.c   xrMarkApiClass reach-through (PICO runtime)
               │     └─ vdhs_battery.c controller-battery input
               └─ VdHsMod.dll         managed mod (netstandard2.0, reflection only)
                     └─ VdHsMod.Loader.Start()
```

- `zygisk/jni/module.cpp` is the injector. It targets the real package name
  `VirtualDesktop.Android`, exempts the payload fds across app specialization,
  copies the payload into the app's private data dir and calls the native entry
  point. The bundled hand mesh is installed only when missing, preserving a
  user's custom mesh. No C++ STL is used (Zygisk requires `APP_STL=none`).
- `mod/native/` builds `libvdhs.so` and the Shell receiver `libvdhs_shell.so`. Both use
  the thin `vdhs_sym.c` adapter over the already-vendored xDL; symbol batches
  select one mapped object and never force-load a second library or fill gaps
  from unrelated APKs. No mapped-object handle is cached across runtime reloads.
- `mod/src/VdHsMod/` is loader-agnostic C#: it holds **zero compile-time
  reference** to any Virtual Desktop / Xenko / Mono.Android assembly. Everything
  it touches is resolved by name through reflection.

`com.picoxr.xrshell` is an additional target in full mode (`2`); other modes
return before staging its separate `libvdhs_shell.so`. VD's Input-tab checkbox
is the only gesture-suppression control. Its saved app-private value is mirrored
once per second to `vdhs.system_gestures`, with property writes only on changes.
The Shell hook suppresses gestures only for focused VD with the exact value `1`;
missing or invalid state is off, and other applications keep their original path.
No module-level gesture setting or managed environment gate is used. Install and
reboot once to load the receiver; subsequent UI changes need no restart.
At Shell process start the injector keeps only the payload descriptor. A
background thread waits for `sys.boot_completed=1`, then stages the payload and
installs the hook; the gesture-state mirror also starts after boot completion.
The hook is not installed in the OpenXR runtime service. Its private libc++
layout is checked against the Shell's known streaming-app entry before hooking.
The in-app gesture switch is independent of the startup capability helper.

### VD-only system launch capability

`coldstart/java/org/vdpico/capability/` is the existing Android-10 metadata
helper. Before specialization, Zygisk's server callback reads the saved choice
and the separate DEX; the server's boot thread only starts a native worker and
returns. After `sys.boot_completed=1` that worker attaches to the VM, loads the
DEX with `InMemoryDexClassLoader` and starts the helper threads. No VD, OpenXR or
Mono payload is loaded into system_server. The post-fs-data script does nothing,
so boot performs no setting rewrite or `sync`.

On the verified PICO 4 Pro / OS 5.13.7 fingerprint, full mode enables the helper
by default, unless an explicitly saved disable choice exists. It copies VD's
PMS metadata Bundle and adds only integer `handtracking=1`, then confirms it
through the normal PackageManager getter. Disabling restores its owned key and
original type/value without replacing later edits to other keys; external
ownership conflicts are preserved. A replacement VD package gets a new baseline.
The fixed root-only socket and `coldstart.sh` expose status, independent query,
enable and disable. No global hand-tracking property or runtime event hook is
changed by this helper. PicoHand<->Knuckles below does not
run inside system_server and does not share this startup declaration.

`vdhs_caps.c` separates the firmware's `ro.pxr.eyetracking.support` ET/FT hint
from the runtime's enabled `XR_EXT_eye_gaze_interaction` system support result.
Only confirmed runtime gaze support enables gaze mutations. An explicit hardware
`0` disables the legacy PICO face-permission alias; an absent property retains
that existing workaround without advertising or creating a face tracker. VD's
PICO path does not enable FB/ANDROID face extensions, so their untouched property
fields are not used to reject Pro face tracking. Hand tracking and passthrough
are independent of these gates.

## 2. The OpenXR compatibility layer

Virtual Desktop ships its own Khronos OpenXR loader
(`libopenxr_loader.so`) and talks to the PICO runtime through it. The layer
installs **ShadowHook inline hooks on that loader's exports** — no system
library is touched, and the hooks come from the in-memory symbol table, so they
track the installed build:

| Loader export | What the hook does |
|---|---|
| `xrEnumerateInstanceExtensionProperties` | On the first call: force the PICO runtime to load, then `xrMarkApiClass`. |
| `xrCreateInstance` | Mark fallback, append the PICO controller extension, strip layer-only extensions. |
| `xrGetInstanceProcAddr` | Extension-function wrapper table (serves the FB/EXT hand functions). |
| `xrCreateAction` / `xrGetActionStatePose` | Track `grip_pose`; report the controller pose inactive while PICO's active input is the hand. |
| `xrGetSystemProperties` | Log hand-tracking support and observe eye-gaze support for capability gating. |
| `xrEndFrame` / `xrPollEvent` | Layer-list / passthrough event handling and permission fixes. |
| `xrCreateActionSpace` / `xrLocateSpace` | Eye-gaze logging and pointer-only filtering / coordinate compensation. |
| `xrReleaseSwapchainImage` (and friends, see `vdhs_pt.c`) | Hand-passthrough hole (below). |

### 2.1 Enabling the PICO hand-tracking extensions

PICO's runtime only advertises `XR_PICO_hand_tracking` and friends after the
vendor loader calls `xrMarkApiClass(mask)`; VD's bundled loader never does. The
layer reaches the runtime through the same system forward loader named in
`/vendor/etc/openxr/1/active_runtime.json` and calls the mark at the right point
in VD's initialization. It also appends the extension to VD's managed extension
list (and teaches VD's enum name cache the new name), because VD filters the
runtime list through its own whitelist before enumerating.

### 2.2 Aim, pinch and menu

When the runtime does not supply an aim state, the layer synthesises
`XR_FB_hand_tracking_aim` from the joints: pinch detection, a
shoulder→index-proximal pointing ray, and a palm-toward-headset menu gesture. If
the runtime's own aim is valid it is used instead.

### 2.3 Hand mesh

`xrGetHandMeshFB` is served from `hand_mesh_fb.bin`, an OpenXR `XR_FB_hand_tracking_mesh`
blob converted offline from the headset's own PICO hand mesh. The module bundles
the blob from `assets/mesh/` and installs it automatically when missing. Its
provenance and redistribution disclaimer are in `assets/mesh/README.md`.
Without a usable blob, the mesh extension is not advertised. See
`tools/mesh/xrshell_mesh.py`.

### 2.4 Hand passthrough

- **In Virtual Desktop itself**, each hand is a separate projected layer (PICO
  keeps one geometry per layer); the background passthrough state is restored
  when a hand hole is not drawn.
- **In SteamVR**, the layer does not create a layer or copy an image. Inside
  `xrReleaseSwapchainImage`, before VD's own VR texture goes to the compositor,
  it sets the texture's alpha to 0 inside one scissor-bounded disc per hand and
  masks the RGB. The disc follows VD's own Quest passthrough window. It only
  runs when VD's own `PassthroughPortals` logic says hand passthrough should be
  visible (VR stream source, hands as current input, VR passthrough hands
  enabled, etc.), so it never opens a hole the official app would not.

## 3. Eye gaze

Eye-gaze code remains in the build and is enabled by detected capability rather
than a device-model whitelist. Capability gating does not change the
pointer/streaming separation described below.

The layer's additional One-Euro smoothing, jump confirmation and invalid-sample
hold (`gaze_filter`) apply only to the UI pointer. `gaze_vd_fix` pre-compensates
the head pose that Virtual Desktop drops for that pointer. Both settings default
to `1` in v1.0.4 and can be disabled independently in `hand_gesture.txt`
(see `config/hand_gesture.example.txt`).

VD uses the same OpenXR gaze-space lookup for its world-space pointer and its
head-relative streaming data. Before processing a sample, the layer uses Mono's
no-IL stack walker to identify the immediate managed consumer:

- `Session.TryGetEyeGazePose` called by `OpenXRHMD.Update`: pointer processing.
- `OpenXRHMD.GetEyeState`: pass through for foveated streaming / tracking forwarding.
- Unknown callers or unavailable metadata: pass through.

The walker and its metadata callbacks run inside a balanced Mono GC-unsafe
region, restoring the incoming thread state afterward. P/Invoke threads are
attached but GC-safe; a cold AOT stack lookup can contend on a cooperative mutex
and must not attempt a second GC-safe transition from that state. If either
transition API is unavailable, classification returns unknown without walking.

Streaming returns immediately after classification, retaining the original
upstream pose and validity flags, with no extra filter, jump confirmation, sample
hold or coordinate compensation. It also skips the pointer diagnostic's extra
head/space queries, even with `gaze_log=1`. It continues through VD's native
projection and sending logic. Thread ID and OpenXR space
arguments are not used to guess intent: tracking forwarding can also call
`GetEyeState` on the render thread. Filter history and gaze helper/log state are
thread-local, so streaming queries cannot advance a pointer's filter history.
One-time per-thread route logs report `filter_applied` and `vd_fix_applied`;
streaming must report both as `0`. Per-sample pointer diagnostics observe the
pose before coordinate compensation.

v1.0.3 processed both consumers. With that release, disable both `gaze_vd_fix`
and `gaze_filter` for unmodified streaming gaze. The routing fix in v1.0.4
has passed one on-device observation on PICO 4 Pro / VD 1.34.22.0: with both
settings enabled, logs showed pointer processing `1/1` and head-relative
processing `0/0` on both the streaming and render threads. The user reported
normal foveated-region following and menu-pointer behaviour.

## 4. Controller battery and PicoHand<->Knuckles

`vdhs_battery.c` adds a left/right float input action to VD's existing controller
action set. Its optional loader hooks append battery bindings to VD's PICO
interaction-profile suggestion and sample after `xrSyncActions`. A rejected
battery binding leaves VD's original input bindings intact.

Samples are cached for one second. The native Mono helper in `vdhs_hand.c`
resolves the nested `Game._frameData.DeviceState` controller-battery offsets
from metadata, obtains the current Game object inside a balanced GC-unsafe
region, and writes only the two signed bytes. It does not retain a movable
object pointer or replace a frame containing concurrently updated poses.
VD's serializer, compression and PC transport remain unchanged.

An inactive or failed battery action yields unavailable readings, not zero.
Unavailable Mono metadata skips the battery write without changing the original
Sync return or controller input. The PC patch supplies the corresponding
SteamVR properties on VD's existing physical-controller containers, retaining the
last known percentage while temporarily withdrawing battery availability.

### 4.1 Battery-only PC capsule

`pc/driver/src/` builds a freestanding x64 capsule containing only this project's
code. `NativePatch.cs` verifies the supported VD original, appends RX code and RW
state sections, and merges unwind/relocation records locally. It changes a single
provider slot, RunFrame, to publish battery. The original Init and Cleanup and the
original controller-creation and update paths are left as they were. Original
tracker/input handlers still run; no second SteamVR driver or device is
registered. The capsule carries battery publication only and has no
hand-switching code. It contains no original VD tables or complete VD image, and
no VD DLL, original or patched, is distributed. Installation keeps the exact
original and a receipt beside it.

### 4.2 PicoHand<->Knuckles

PicoHand<->Knuckles is optional and runs independently of VD and the base
module. Two components talk directly over the local network:

```
SteamVR ── driver_picomixedvr (PC)  ──LAN──▶  headset daemon (root)
             samples Index input                applies / restores the
             decides pickup / putdown           system hand-tracking setting
```

Sources are in `knuckles/pc/`, `knuckles/headset/` and `knuckles/capture/`.
Some internal identifiers, such as the driver name `picomixedvr` and the
headset state root, keep their earlier names so that installed state stays
valid across upgrades; users do not manage them.

**PC driver.** An ordinary SteamVR driver with no virtual device. From the
driver context it samples the trackpad touch of real Lighthouse Index
controllers through the public input interface, together with the same device's
pose. The capture source in `knuckles/capture/` is compiled into the same
`driver_picomixedvr.dll`; there is no second capture plugin. It reads the public
DriverInput003/004/005 Boolean Create/Update prefixes, deduplicating shared tables,
and involves no Virtual Desktop addresses or code. The
driver needs no ADB, VD frame or resident PC application, and starts and stops
with SteamVR.

Policy:

- Pickup needs trackpad touch and valid motion on the same real controller.
  Touch from one controller and motion from the other do not combine.
- Putdown needs every present Index controller to be explicitly released and
  stationary for 1000ms. A controller that is connected with unknown touch or
  lost tracking is not released.
- Losing all verified sources restores the original state and returns to
  standby; it never synthesizes a release.
- With no Index controllers the driver only requests daemon status. It writes no
  hand-tracking preference and no system setting.
- The preference that gives hands priority is applied only after a real input
  decision and a successful status exchange, and is restored to its original
  value on exit.

**Headset daemon.** A root service in its own Magisk module with its own state
directory. It listens on the headset's current private wlan0 address. The saved
paired PC address remains valid; a paired discovery request also grants a new
same-subnet PC address a five-second connection window while no client or pending
recovery owns the service. The shared key is stored in the pairing directory
(readable only by the current Windows user and SYSTEM). Requests carry a session id and a
rising sequence number, and each is answered with an execution receipt that
reports what the system backend actually did. On both PICO 4 and PICO 4 Pro,
the backend requires the exact fingerprint of the verified PICO OS 5.13.7 build.
It uses system hand-tracking control and does not require eye- or face-tracking
hardware.

The daemon reuses the system hand-tracking control and recovery backend. It saves
the original configuration to a recovery journal before changing it and restores
it on an explicit restore, a lost connection, or expiry of the 3000ms mode lease.
The driver keeps a roughly 500ms heartbeat while it holds a mode. A restore
failure keeps the saved baseline and stops control instead of retrying blindly.

**Discovery and address changes.** The PC sends a 102-byte `PKD1` UDP request to
the saved address and to `239.255.255.250:29744` on active private Ethernet/Wi-Fi
interfaces. It includes the existing plaintext pairing key and a fresh random
nonce. A matching `PKA1` reply echoes both; the PC selects the reply's source
address and then uses the unchanged TCP control protocol. This is for a trusted
LAN, not an encrypted or authenticated replacement for TCP. Face-tracking Bridge
port 9030 is not used, and Bridge need not be installed or running.

The daemon receives unicast on the explicit wlan0 address and multicast on the
group address, with membership on wlan0; neither listener binds `0.0.0.0`. It
checks wlan0 once per second. A new address closes the old client and restores
the original hand-tracking state before serving control on the new address.
Failed binds are retried without deleting the supervisor's ready marker. Runtime
address changes do not rewrite pairing files.

While the saved choice is auto, the provider retries transient network failures
with fresh discovery/session state after a two-second pause. It also resumes
after SteamVR standby. Stop/restore cancels an outstanding search; helper errors,
protocol errors and unfinished HAND recovery do not auto-rearm. A valid discovery
reply is required for a changed address; if discovery gets no reply, the provider
tries the saved address for compatibility with older daemons.
All discovery polling runs on the existing worker, not SteamVR's frame callback.

**Boot and legacy migration.** The standalone boot script waits for
`sys.boot_completed=1` and checks the saved enable choice. If the legacy state
directory and its management script are present, it synchronously calls the old
`disable` operation before enabling and starting the new service. The base module
may call that same operation earlier at late_start. Legacy `stop`, `disable` and
`restore` operations hold a separate management file lock for the whole recovery
transaction, distinct from the native worker and supervisor locks; recovery
children do not inherit it. After recovery succeeds, the standalone script checks
the saved choice and module disable/removal markers again. The native migration
guard is unchanged, and a fresh standalone installation needs no base module.

**Installation.** The unified installer treats it as a separate item, selected
with `-WithKnuckles`. Fresh installs add only the base unless `-WithKnuckles` is
selected; upgrades keep an installed optional module. The base never touches the
system hand-tracking setting. The flag needs a full installation and is rejected
together with `-HeadsetOnly` or `-Mode 1`. When selected, the installer registers
the driver, installs the daemon module and pairs the two once over a private LAN
address pair, detected or given with `-HeadsetAddress` and `-PcAddress`; the
pairing is stored for the calling Windows user and SYSTEM only. An installation
receipt in that directory records that the module is installed, and the saved
enabled or disabled choice is kept across upgrades. During installation, existing
legacy state is retired through the newly staged base manager when available,
or the active manager for an optional-only installation. A failed recovery stops
installation before the new owner is installed. `-Action Disable` and
`-Action Enable` act on this module alone,
`-Action Configure` refreshes pairing when discovery is unavailable, and `-Action Status` reads
the saved configuration without contacting the headset. `-Action UninstallKnuckles`
restores hand tracking, unregisters the driver and marks the headset module for
removal while the base module and battery display stay; `-Action Uninstall` does
that first and then removes the rest. The headset module's boot script starts the
daemon according to the saved choice. Instructions:
[Installation and use](PC-SETUP.md).

## 5. Managed mod

`VdHsMod.dll` supplies managed extension-list changes, the in-app gesture switch
and localization. The native layer handles PICO hand/controller pose selection.
The managed backend is chosen through `payload/backend.txt` / `VDHS_BACKEND`:

### 5.1 Backends

| Backend | What it does | State |
|---|---|---|
| `auto` (default) | In the default build, redirect if a patched `Xenko.OpenXR.dll` is staged, else dataonly. | — |
| `redirect` | Preload a locally produced, MVID-bumped `Xenko.OpenXR.patched.dll` so VD uses it instead of its store copy. Survives the APK's AOT images. | experimental; needs a patched DLL you build yourself |
| `dataonly` | Reflection-only data injection: enable the PICO extension but do not change `IsControllerActive`. | default without a redirect artifact |
| `none` | Skip backend changes; in full mode the native layer and managed UI features still run. | not an injection-only control |

None of these backends modify the installed APK; `redirect` only loads an
assembly you already produced for your own device. Use mode `1`, not backend
`none`, for an injection-only control run. Restart VD after changing the backend.

### 5.2 UI switch and localization

`GestureToggle` creates the Input-tab checkbox at runtime and saves its value in
the app-private `system_gestures.txt`. `service.sh` mirrors this value to the Shell;
the module does not need a second gesture-enablement setting.

`UiLocalize` reads `InstalledUICulture` at startup and loads `ui_zh.txt` only for
Chinese system languages. It initializes the cached control-panel pages,
templates and separate performance-overlay page once their resources are ready.
Each text binding retains its original font and actual owner/root. User data,
telemetry, device names and units remain separate from fixed captions.

Text-change events translate UI-thread updates synchronously; background updates
are queued to the UI thread. Each panel redraws its own render target. Repeated
captions with the same translation and font skip an extra full redraw, while
asynchronous updates retain it to clear any English that may have rendered.
Mixed fields restore their original font when returning to numbers or preserved
names. Page recreation releases the old listeners and initializes the replacement;
the initialization worker exits rather than scanning labels continuously.

## 6. Configuration and rollback

- `payload/mode.txt`: `1` = injection-only control,
  `2` = full layer (default). Mode `1` checks that Mono is reachable without
  starting the native compatibility layer or loading the managed mod. Reboot
  the headset after changing modes so the Shell receiver, startup helper and
  gesture-state mirror use the same mode.
- `payload/backend.txt` (and `VDHS_BACKEND`): managed backend, above.
- `<app_data>/vdhs/hand_gesture.txt`: the runtime feature toggles.

No feature configuration file is needed for a fresh installation. Tracking
defaults include `pt_bg_fix`, `pt_split`, `pt_hole`,
`gaze_filter`, `gaze_vd_fix`, and the 300 ms joint freeze gate. SteamVR drawing
still follows VD's own settings (`pt_follow_settings=1`). Diagnostic switches
(`gaze_log`, `hj_diag_ms`, `pt_alpha_only`, `pt_style`) default off. Existing explicit
configuration values take precedence; the module never rewrites that file.
Gesture suppression defaults off, localization follows the system language, and
the startup helper defaults on only for its verified firmware fingerprint.

## 7. Initialization and hook failure handling

`VdHsMod.Loader.Start()` catches and logs managed exceptions. The native payload
uses a detached worker to wait for Mono and load the managed mod, rather than
waiting for the runtime in the app-specialization callback.

Native loader hooks become active together only after all required hooks are
installed. A failed installation rolls back its hooks; callbacks remain
pass-through until activation, including any hook that could not be removed.
The optional swapchain hooks use the same all-or-nothing activation, so a
partial installation cannot enable alpha-hole drawing or its layer flags.
Once native-layer initialization starts, the managed payload and input probe
leave runtime negotiation to VD-thread hooks, including after installation fails.

## 8. Scope and limitations

- Targets: PICO 4 and PICO 4 Pro, arm64, Virtual Desktop Android 1.34.22.0. Internal
  layout/names are version-specific; a Virtual Desktop update can break the
  layer.
- Root + Zygisk are required. Full mode loads code into VD and PICO Shell, and
  loads the startup helper into system_server on its supported firmware. The
  APK, manifest, signature and package name remain unchanged.
- It is a community interoperability project and is not affiliated with Virtual
  Desktop or PICO.
