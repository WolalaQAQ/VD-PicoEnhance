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
   └─ zygisk/arm64-v8a.so            injector, matches VirtualDesktop.Android
         └─ stages payload/ into <app_data>/vdhs/ at pre-app-specialize
         └─ dlopen(libvdhs.so) -> vdhs_payload_main()
               ├─ libvdhs.so          native OpenXR compatibility layer
               │     ├─ vdhs_layer.c  inline hooks on VD's bundled libopenxr_loader.so
               │     ├─ vdhs_hand.c   hand tracking, aim/mesh, managed extension list
               │     ├─ vdhs_pt.c     zero-copy hand-passthrough hole
               │     └─ vdhs_mark.c   xrMarkApiClass reach-through (PICO runtime)
               └─ VdHsMod.dll         managed mod (netstandard2.0, reflection only)
                     └─ VdHsMod.Loader.Start()
```

- `zygisk/jni/module.cpp` is the injector. It targets the real package name
  `VirtualDesktop.Android`, exempts the payload fds across app specialization,
  copies the payload into the app's private data dir and calls the native entry
  point. No C++ STL is used (Zygisk requires `APP_STL=none`).
- `mod/native/` builds a single shared payload, `libvdhs.so`.
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
| `xrCreateActionSpace` / `xrLocateSpace` | Eye-gaze space logging. |
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
blob converted offline from the headset's own PICO hand mesh. That blob is a
PICO system resource and is **not** committed here; without it, the mesh
extension is simply not advertised. See `tools/mesh/xrshell_mesh.py`.

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

The layer smooths the PICO gaze with a One-Euro filter plus a jump-confirmation
step, and compensates the head pose that Virtual Desktop drops when it composes
the PICO gaze. Pointer motion settings are configurable (see
`config/hand_gesture.example.txt`).

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

## 5. Configuration and rollback

- `payload/mode.txt` (and `VDHS_MODE`): `1` = injection smoke test only
  (rollback), `2` = full layer. The module ships `1` so the very first boot
  proves injection without touching the managed runtime.
- `payload/backend.txt` (and `VDHS_BACKEND`): managed backend, above.
- `<app_data>/vdhs/hand_gesture.txt`: the runtime feature toggles.

## 6. Robustness contract

The mod must never take down the host process. `VdHsMod.Loader.Start()` is fully
guarded: every step degrades to a no-op on failure, and the native payload does
its work on a detached thread rather than blocking VD's main thread.

## 7. Scope and limitations

- Target: PICO 4 Pro, arm64, Virtual Desktop Android 1.34.22.0. Internal
  layout/names are version-specific; a Virtual Desktop update can break the
  layer.
- Root + Zygisk are required. The module does not touch the APK, the manifest,
  the signature or the package name, and does not modify other processes.
- It is a community interoperability project and is not affiliated with Virtual
  Desktop or PICO.
