# mod/ — VdHsMod and the native payload

`VdHsMod` is the managed mod that installs the hand/controller hot-switch
behaviour. It is loader-agnostic and holds **zero compile-time reference** to any
Virtual Desktop / Xenko / Mono.Android assembly: every handle into the app is
resolved by name through reflection.

```
mod/
  src/VdHsMod/        C# class library (netstandard2.0)
  native/             shared native payload libvdhs.so
    vdhs_mark.c       canonical xrMarkApiClass(0x3b) reach-through
    vdhs_payload.c    Mono embedding loader (vdhs_payload_main)
    vdhs_layer.c      OpenXR compatibility layer (inline hooks)
    vdhs_hand.c       hand tracking, aim/mesh, managed extension list
    vdhs_pt.c         zero-copy hand-passthrough hole
    CMakeLists.txt
  tools/bump_mvid.cs  helper to MVID-bump a redirect artifact
```

## Build

```powershell
# managed mod: offline-safe default (redirect + dataonly backends, no NuGet)
dotnet build mod/src/VdHsMod/VdHsMod.csproj -c Release

# native payload + Zygisk module, staged into zygisk/payload/
pwsh -File zygisk/build.ps1
```

The full module is assembled by `zygisk/build.ps1`, which stages `libvdhs.so`,
`VdHsMod.dll` and the bundled hand mesh into `zygisk/payload/`.

## Backends (runtime switch, no rebuild)

Set `VDHS_BACKEND` or edit `zygisk/payload/backend.txt`:

| backend | what it does | state |
|---|---|---|
| `auto` | redirect artifact if staged, else dataonly | default |
| `redirect` | preload a pre-patched `Xenko.OpenXR.patched.dll` with a bumped MVID, so it survives the official APK's AOT images | experimental; needs a patched DLL you produce yourself |
| `dataonly` | reflection-only data injection (enables the PICO extension, cannot change `IsControllerActive`) | safe fallback |
| `none` | injection smoke test only | — |

## Redirect artifact

The `redirect` backend loads an assembly you produced for your own device; it is
an optional, experimental route and is not required for the shipped Zygisk
compatibility layer. Its only build helper here is the MVID bump:

```powershell
# turn a locally patched Xenko.OpenXR.dll into a redirect artifact
dotnet run mod/tools/bump_mvid.cs -- <patched.dll> mod/redirect/Xenko.OpenXR.patched.dll

# stage it
pwsh -File zygisk/build.ps1 -Redirect mod/redirect/Xenko.OpenXR.patched.dll
```

Bumping the MVID makes the official AOT image no longer match, so Mono JITs the
assembly instead. `mod/redirect/` is a build-output directory and is not tracked.
