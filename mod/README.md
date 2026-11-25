# `mod/` — Managed Mod (`VdHsMod`) and Native Payloads

**English** | [简体中文](README_zh.md)

`mod/` contains both the native C payloads (`libvdhs.so` and `libvdhs_shell.so`) and the managed C# mod (`VdHsMod.dll`) staged into Virtual Desktop at runtime.

`VdHsMod` targets `netstandard2.0` and holds **zero compile-time references** to any Virtual Desktop, Xenko, or `Mono.Android` assembly—every type, member, and event is resolved by name through reflection.

## Directory Layout

```text
mod/
├── src/VdHsMod/                  C# managed mod (netstandard2.0, offline-safe default)
│   ├── Loader.cs                 Entry point invoked by libvdhs.so
│   ├── Backends.cs               Backend selection (auto / redirect / dataonly / none)
│   ├── ExtensionEnabler.cs       Managed OpenXR extension-list injection
│   ├── GestureToggle.cs          Runtime "Suppress system gestures" checkbox + UI dispatch
│   ├── UiLocalize.cs             Automatic Simplified Chinese UI & overlay localization
│   ├── StreamingLimits.cs        3× bitrate & extended VR quality slider unlock
│   ├── RedirectBackend.cs        Optional pre-patched assembly preloader
│   ├── AssemblyRedirect.cs       Assembly resolve hook for redirect backend
│   ├── PayloadConfig.cs          Reads backend.txt / VDHS_BACKEND
│   ├── VdReflection.cs           Reflection helpers
│   ├── NativeBridge.cs           Managed-to-native glue
│   └── VdHsMod.csproj
├── native/                       Native C11 payloads (built with Android NDK + CMake/Ninja)
│   ├── vdhs_payload.c            Mono embedding loader (vdhs_payload_main)
│   ├── vdhs_layer.c              OpenXR loader inline hooks (ShadowHook)
│   ├── vdhs_hand.c               Hand tracking, FB aim/mesh, joint freeze gate, Mono helpers
│   ├── vdhs_pt.c                 Zero-copy SteamVR hand-passthrough alpha hole
│   ├── vdhs_mark.c               xrMarkApiClass(0x3b) runtime reach-through
│   ├── vdhs_battery.c            Controller battery OpenXR action & sampling
│   ├── vdhs_caps.c               Runtime eye-gaze / face capability gating
│   ├── vdhs_sym.c                Thin xDL mapped-symbol resolver (shared by both .so libs)
│   ├── vdhs_shell.c              PICO Shell gesture-suppression receiver (libvdhs_shell.so)
│   ├── third_party/shadowhook-2.0.1/  Vendored ShadowHook 2.0.1 + xDL
│   └── CMakeLists.txt
└── tools/
    └── bump_mvid.cs              Helper to bump the MVID of a local redirect artifact
```

## Building

```powershell
# Build only the managed mod (offline-safe default: no NuGet restore)
dotnet build mod/src/VdHsMod/VdHsMod.csproj -c Release

# Build VdHsMod.dll, libvdhs.so, libvdhs_shell.so, coldstart helper, and Zygisk injector
pwsh -File zygisk/build.ps1

# Build and pack zygisk/dist/vdhs_zygisk.zip
pwsh -File zygisk/build.ps1 -Pack
```

`zygisk/build.ps1` compiles all components with `-Wall -Wextra` (zero warnings expected) and stages `libvdhs.so`, `libvdhs_shell.so`, `VdHsMod.dll`, and the bundled `hand_mesh_fb.bin` into `zygisk/payload/`. Additional flags:

- `-Clean`: Wipe CMake build directories before configuring.
- `-Harmony`: Opt-in legacy Harmony backend build (`-p:VdHsHarmony=true`, restores `Lib.Harmony 2.3.5` from NuGet and stages `0Harmony.dll`).
- `-RedirectArtifact <path>`: Stage a locally built `Xenko.OpenXR.patched.dll` into `zygisk/payload/`.

## Managed Backends

Switch backends at runtime without rebuilding by editing `zygisk/payload/backend.txt` (or setting the `VDHS_BACKEND` environment variable) and restarting Virtual Desktop:

| Backend | Behavior | Notes |
|---|---|---|
| `auto` | Uses `redirect` if `Xenko.OpenXR.patched.dll` is staged, else `harmony` if built and staged, else `dataonly` | Default |
| `redirect` | Preloads a locally built, MVID-bumped `Xenko.OpenXR.patched.dll` so Mono JITs it instead of using the APK's AOT image | Experimental; user-built |
| `dataonly` | Reflection-only extension injection without modifying `IsControllerActive` | Default fallback |
| `none` | Skips backend changes; native OpenXR layer and managed UI features still run in full mode | Not an injection smoke test—use `payload/mode.txt = 1` + reboot for a control run |

## Building a Local Redirect Artifact (Optional)

The `redirect` backend is an optional, experimental route not required by the shipped Zygisk compatibility layer. To convert a locally patched `Xenko.OpenXR.dll` into a redirect artifact whose MVID no longer matches the APK's AOT image:

```powershell
# Bump the MVID so Mono JITs the replacement assembly
dotnet run mod/tools/bump_mvid.cs -- <patched.dll> mod/redirect/Xenko.OpenXR.patched.dll

# Stage it into the module tree
pwsh -File zygisk/build.ps1 -RedirectArtifact mod/redirect/Xenko.OpenXR.patched.dll
```

`mod/redirect/` is gitignored build output.
