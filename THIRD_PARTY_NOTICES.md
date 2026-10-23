# Third-party notices

VD-PicoEnhance itself is MIT-licensed (see [LICENSE](LICENSE)). It vendors or
references the following third-party components. Their notices are reproduced
below and their license files are kept next to the code.

**No Virtual Desktop, Xenko or Mono binaries or source are bundled in this
repository.** The compatibility layer interacts with those components only at
runtime, through public/observable interfaces present on the device. The base PC
installer patches a verified driver on the user's computer to publish controller
battery; its original code and tables are not part of the distributed capsule.

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

## Valve OpenVR headers (PC build-time dependency)

- Upstream: <https://github.com/ValveSoftware/openvr>, commit `91825305130f446f82054c1ec3d416321ace0072` (v2.12.14).
- The PC builds fetch or accept the pinned `openvr_driver.h` for ABI declarations; no OpenVR runtime library is redistributed. This covers both the base battery capsule and the optional PicoHand<->Knuckles provider.
- License: BSD-3-Clause, reproduced in `pc/driver/OPENVR-LICENSE.txt` and included with the battery capsule, and in `OPENVR-LICENSE.txt` beside the PicoHand<->Knuckles provider.

## Zig 0.13.0 runtime libraries (PicoHand<->Knuckles provider)

- Upstream: <https://ziglang.org/> (<https://github.com/ziglang/zig>), release 0.13.0.
- The optional PicoHand<->Knuckles SteamVR provider, `driver_picomixedvr.dll`, is compiled and linked with the Zig C++ toolchain. The compiler itself is a build-time tool and is not distributed. The DLL does contain code from the libraries Zig links in, including libunwind, and for those this project redistributes their license texts.
- Licenses:
  - Zig, MIT.
  - LLVM libc++, libc++abi and libunwind, Apache License 2.0 with LLVM Exceptions.
  - mingw-w64 runtime, mostly Zope Public License 2.1, with parts marked as public domain, BSD or LGPL as set out in its `COPYING`.
- The verbatim license files from the Zig 0.13.0 package (`LICENSE`, `lib/libcxx/LICENSE.TXT`, `lib/libcxxabi/LICENSE.TXT`, `lib/libunwind/LICENSE.TXT` and `lib/libc/mingw/COPYING`) are collected in `ZIG-RUNTIME-NOTICES.txt`. In the release bundle it sits at `knuckles/pc/driver_picomixedvr/ZIG-RUNTIME-NOTICES.txt`, and the installer places it with the driver on the PC.

## PICO hand mesh (`assets/mesh/hand_mesh_fb.bin`)

This is **not** MIT-licensed third-party code; it is a device-derived binary
asset, included under the terms below.

- Origin: converted from the headset's own PICO system resource
  `assets/mesh/HAND_{L,R}_OpenXR.MESH/` inside `/system/priv-app/XRShell/XRShell.apk`.
- Tool: `tools/mesh/xrshell_mesh.py` (this project, MIT), run offline. No PICO
  library is linked at build or run time.
- Purpose: provides the mesh the compatibility layer serves for
  `XR_FB_hand_tracking_mesh`. It is bundled in the Magisk module and also offered
  beside it in the release bundle for the installer.
- Status: `XRShell.apk` and its mesh are the property of PICO. This file is
  redistributed solely to make an interoperability feature of an unmodified
  third-party application work, with no affiliation with or endorsement by PICO,
  and it will be removed on request. You can delete it and either generate your
  own copy from your device or run without the hand mesh.
