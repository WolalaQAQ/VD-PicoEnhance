# Architecture

VD-PicoEnhance adds PICO hand tracking, hand passthrough and eye-gaze fixes to
the **unmodified** Virtual Desktop app. It does not patch, repack, re-sign or
rename the APK. Everything happens at runtime, inside the Virtual Desktop
process.

This document describes the shipped Zygisk route. It is written for readers who
want to understand or build on the code; it is not required to install the
module.

## 1. Components

```
Magisk / Zygisk
   └─ zygisk/arm64-v8a.so            injector, three matched processes
         └─ stages payload/ into <app_data>/vdhs/ at pre-app-specialize
         │
         ├─ VirtualDesktop.Android
         │     └─ dlopen(libvdhs.so) -> vdhs_payload_main()
         │           ├─ libvdhs.so          native OpenXR compatibility layer
         │           │     ├─ vdhs_layer.c  inline hooks on VD's bundled libopenxr_loader.so
         │           │     ├─ vdhs_hand.c   hand tracking, aim/mesh, managed extension list
         │           │     ├─ vdhs_pt.c     zero-copy hand-passthrough hole
         │           │     └─ vdhs_mark.c   xrMarkApiClass reach-through (PICO runtime)
         │           └─ VdHsMod.dll         managed mod (netstandard2.0, reflection only)
         │                 └─ VdHsMod.Loader.Start()
         │
         ├─ com.pico.xr.openxr_runtime
         │     └─ dlopen(libvdhs_rt.so)    -> vdhs_rt_main()
         │           └─ vdhs_rt.c          clears the gesture state the runtime relays
         │
         └─ com.picoxr.xrshell
               └─ dlopen(libvdhs_shell.so) -> vdhs_shell_main()
                     └─ vdhs_shell.c       makes the shell skip gestures for VD
```

- `zygisk/jni/module.cpp` is the injector. It matches the real package names
  `VirtualDesktop.Android`, `com.pico.xr.openxr_runtime` and `com.picoxr.xrshell`,
  exempts the payload fds across app specialization, copies the payload into the
  app's private data dir and calls the matching native entry point. The bundled
  hand mesh is installed only when missing, preserving a user's custom mesh. No
  C++ STL is used (Zygisk requires `APP_STL=none`).
- `mod/native/` builds three payloads: `libvdhs.so` (Virtual Desktop),
  `libvdhs_rt.so` (the OpenXR runtime process) and `libvdhs_shell.so` (the system
  shell). They share `vdhs_sym.c`, the in-memory ELF resolver.
- `mod/src/VdHsMod/` is loader-agnostic C#: it holds **zero compile-time
  reference** to any Virtual Desktop / Xenko / Mono.Android assembly. Everything
  it touches is resolved by name through reflection.

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
| `xrGetSystemProperties` | Log the runtime's hand-tracking support flag. |
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

> **Removed in this PICO 4 (A8110) fork.** The plain PICO 4 has no eye-tracking
> hardware, so the gaze hooks, filtering, compensation and the `gaze_*` settings
> described below were deleted from the native layer and the managed mod. The
> text is kept for upstream reference only.

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

## 4. Managed backends

`VdHsMod.dll` installs the hand/controller hot-switch behaviour. The backend is
chosen at runtime (no rebuild) through `payload/backend.txt` / `VDHS_BACKEND`:

| Backend | What it does | State |
|---|---|---|
| `auto` (default) | redirect if a patched `Xenko.OpenXR.dll` is staged, else dataonly. | — |
| `redirect` | Preload a locally produced, MVID-bumped `Xenko.OpenXR.patched.dll` so VD uses it instead of its store copy. Survives the APK's AOT images. | experimental; needs a patched DLL you build yourself |
| `dataonly` | Reflection-only data injection: enable the PICO extension but do not change `IsControllerActive`. | safe fallback |
| `none` | Injection smoke test only. | — |

None of these backends modify the installed APK; `redirect` only loads an
assembly you already produced for your own device.

The optional Harmony backend (`build.ps1 -Harmony`) is selected when `auto`
finds no patched DLL but `Lib.Harmony` is available; it patches
`IsControllerActive` in place. Stage a **Mono-compatible** `0Harmony.dll`: the
package's `net35` build binds `mscorlib, Version=2.0.0.0`, which the headset's
Mono does not ship (`TypeLoadException: System.Version`) and the backend silently
falls back to `dataonly`. Use the `netstandard2.0`/`net472` build instead (the
packer now prefers those).

> **PICO 4 (A8110) note.** The layer asks the runtime for the active input device
> first and falls back to the `pvr.active.input_device` system property (`2` =
> hands) when the query is not exported — which is the case for the loader
> Virtual Desktop 1.34.18.0 ships, while 1.34.22.0 resolves it (`r=0`). Either
> way the grip pose's `isActive` is masked, and that native path alone makes VD
> render hands, so `auto`/`dataonly` is sufficient — the Harmony
> `IsControllerActive` patch is **not** required on this device.

## 5. Configuration and rollback

- `payload/mode.txt` (and `VDHS_MODE`): `1` = injection smoke test only
  (rollback), `2` = full layer. The module ships `2` (the full layer); set it to
  `1` for a control run that leaves the managed runtime untouched.
- `payload/backend.txt` (and `VDHS_BACKEND`): managed backend, above.
- `<app_data>/vdhs/hand_gesture.txt`: the runtime feature toggles.

No feature configuration file is needed for a fresh installation. User-facing
features default on, including `pt_bg_fix`, `pt_split`, `pt_hole`,
`gaze_filter`, `gaze_vd_fix`, and the 300 ms joint freeze gate. SteamVR drawing
still follows VD's own settings (`pt_follow_settings=1`). Diagnostic switches
(`gaze_log`, `hj_diag_ms`, `pt_alpha_only`, `pt_style`) default off. Existing explicit
configuration values take precedence; the module never rewrites that file.

## 6. System hand-gesture suppression

The recenter and open-dock gestures are not the app's: the platform tracking
service recognises them, and the system shell (`com.picoxr.xrshell`) performs
them. Nothing inside Virtual Desktop can stop that, so two companion payloads
run outside it.

**Shell (`libvdhs_shell.so`).** This is the mechanism PICO's own streaming app
relies on. `libxrshell.so` keeps

```
std::vector<std::string> gDisableSystemGesturesPackageNames
bool                     gApplicationDisableSystemGestures
InputManager::updateAppDisableSystemGestures(pkg, XrServerDisplayState, bool isSelf)
```

The vector is built in a static initialiser and holds exactly one hardcoded
entry, `com.picovr.picostreamassistant`; `updateAppDisableSystemGestures()` is
called once per frame with the focused package, compares it against that list and
drives the flag, which the gesture handling reads. The payload hooks the function
(by name — the library keeps its full symbol table), answers the check for
`VirtualDesktop.Android` itself and skips the original for that package, so the
flag is stable and the shell's own list is untouched. Scoping is therefore free:
the flag follows the focused app, so gestures return as soon as VD is not it. On
startup it also logs the list it read back, which is how the layout is verified
on a new firmware.

**Runtime (`libvdhs_rt.so`).** PICO's out-of-process OpenXR runtime relays the
platform's gesture state to every session, the shell included. While VD owns the
MR service (`sys.pxr.mrserivce.user`) the payload clears that state, hooking
`InputDeviceManager::service_status_callback` and, once the library is mapped,
the runtime's own emitter for `XR_DEVICE_SYSTEMGESTURE_STATE` (found through the
log string it prints, since the runtime libraries are mapped straight out of
`XRRuntime.apk`). This is a second layer rather than the primary fix.

Both payloads only report; they never write to the app or to system files, and
they stop mattering the moment VD is no longer focused.

## 7. Robustness contract

The mod must never take down the host process. `VdHsMod.Loader.Start()` is fully
guarded: every step degrades to a no-op on failure, and the native payload does
its work on a detached thread rather than blocking VD's main thread.

Native loader hooks become active together only after all required hooks are
installed. A failed installation rolls back its hooks; callbacks remain
pass-through until activation, including any hook that could not be removed.
The optional swapchain hooks use the same all-or-nothing activation, so a
partial installation cannot enable alpha-hole drawing or its layer flags.
Once native-layer initialization starts, the managed payload and input probe
leave runtime negotiation to VD-thread hooks, including after installation fails.

## 8. Scope and limitations

- Target: PICO 4 Pro, arm64, Virtual Desktop Android 1.34.22.0. Internal
  layout/names are version-specific; a Virtual Desktop update can break the
  layer.
- Root + Zygisk are required. The module does not touch the APK, the manifest,
  the signature or the package name, and does not modify other processes.
- It is a community interoperability project and is not affiliated with Virtual
  Desktop or PICO.
