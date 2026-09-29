# Third-party notices

VD-PicoEnhance itself is MIT-licensed (see [LICENSE](LICENSE)). It vendors or
references the following third-party components. Their notices are reproduced
below and their license files are kept next to the code.

**No Virtual Desktop, Xenko or Mono binaries or source are bundled in this
repository.** The compatibility layer interacts with those components only at
runtime, through public/observable interfaces present on the device.

The single device-derived asset that *is* redistributed is the PICO hand mesh
blob (`assets/mesh/hand_mesh_fb.bin`); its origin and disclaimer are documented
below and in `assets/mesh/README.md`.

---

## ShadowHook 2.0.1

- Path: `mod/native/third_party/shadowhook-2.0.1/`
- Upstream: <https://github.com/bytedance/android-inline-hook> (tag `v2.0.1`)
- License: MIT (see `mod/native/third_party/shadowhook-2.0.1/LICENSE`)
- Notes: vendored source, built as a static library. One local change is
  documented in `mod/native/third_party/shadowhook-2.0.1/VENDOR.txt`.

ShadowHook bundles two more MIT/BSD-style projects inside `cpp/third_party/`
(`xdl` and `linux_syscall_support`); their license files are preserved in place.

## Zygisk module API (`zygisk.hpp`)

- Paths: `zygisk/jni/zygisk.hpp`
- Upstream: <https://github.com/topjohnwu/zygisk-module-sample> (`module/jni/zygisk.hpp`)
- Author: John "topjohnwu" Wu
- License: permissive (ISC-style), reproduced in the file header.

The header is vendored verbatim and must not be modified.

## Mono.Cecil (build-time helper only)

- Used by `mod/tools/bump_mvid.cs` (a `dotnet run` file-based script).
- Upstream: <https://github.com/jbevain/cecil>
- License: MIT. Restored from NuGet on demand; not redistributed.

## PICO hand mesh (`assets/mesh/hand_mesh_fb.bin`)

This is **not** MIT-licensed third-party code; it is a device-derived binary
asset, included under the terms below.

- Origin: converted from the headset's own PICO system resource
  `assets/mesh/HAND_{L,R}_OpenXR.MESH/` inside `/system/priv-app/XRShell/XRShell.apk`.
- Tool: `tools/mesh/xrshell_mesh.py` (this project, MIT), run offline. No PICO
  library is linked at build or run time.
- Purpose: provides the mesh the compatibility layer serves for
  `XR_FB_hand_tracking_mesh`. In releases it is offered as a separate file, not
  inside the Magisk module.
- Status: `XRShell.apk` and its mesh are the property of PICO. This file is
  redistributed solely to make an interoperability feature of an unmodified
  third-party application work, with no affiliation with or endorsement by PICO,
  and it will be removed on request. You can delete it and either generate your
  own copy from your device or run without the hand mesh.
