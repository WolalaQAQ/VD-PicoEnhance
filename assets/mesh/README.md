# Hand mesh blob (`hand_mesh_fb.bin`)

`hand_mesh_fb.bin` is the hand mesh the compatibility layer serves for
`XR_FB_hand_tracking_mesh`. Without it the module does not enable Virtual
Desktop's hand-mesh feature.

## What it is

A small, self-describing binary ("VDHM" container) holding, for each hand:

- 26 OpenXR joint bind poses, radii and parent indices,
- the vertex positions / normals / UVs and the index buffer of the hand mesh,
  converted from the headset mesh into OpenXR joint order and output space.

The full layout is documented at the top of `tools/mesh/xrshell_mesh.py`.

## Where it comes from

It is **converted from the headset's own PICO system resource**, not authored by
this project:

1. Source: `assets/mesh/HAND_{L,R}_OpenXR.MESH/` inside
   `/system/priv-app/XRShell/XRShell.apk` (the PICO `XRShell` system app).
2. Conversion: `tools/mesh/xrshell_mesh.py blob`, run offline on the host. No
   PICO library is linked at build or run time.

The blob is therefore a **derivative of a PICO system asset**. It is included
here only so that the interoperability feature (showing the hand model inside
Virtual Desktop) works out of the box.

## Integrity

```
sha256  bc63e23093b8184b510219e78dbe2b67939e87362eab092c634edf2f6e6fca27
size    187200 bytes
```

Releases publish the same blob and list this hash in `SHA256SUMS.txt`.

## How to use it

The blob must sit next to the staged `libvdhs.so`, in the app's private
directory:

```
/data/data/VirtualDesktop.Android/vdhs/hand_mesh_fb.bin
```

The one-shot installer places this file automatically. For manual installation,
launch Virtual Desktop once after installing the module, then close it. On the PC:

```sh
adb push hand_mesh_fb.bin /data/local/tmp/
adb shell
```

In the device shell, enter `su` to obtain root, then run:

```sh
set -e
d=/data/data/VirtualDesktop.Android
owner=$(stat -c %u:%g "$d")
mkdir -p "$d/vdhs"
chown "$owner" "$d/vdhs"
chmod 0700 "$d/vdhs"
cp /data/local/tmp/hand_mesh_fb.bin "$d/vdhs/hand_mesh_fb.bin"
chown "$owner" "$d/vdhs/hand_mesh_fb.bin"
chmod 0644 "$d/vdhs/hand_mesh_fb.bin"
restorecon -RFD "$d/vdhs"
exit
exit
```

The directory must belong to the app so the injector can write its payload on
the next launch. Restart Virtual Desktop afterwards. If the mesh file is absent,
the module does not enable VD's hand-mesh feature.

To generate a blob from your own headset's XRShell APK, install Python and numpy,
then run on the PC:

```sh
python tools/mesh/xrshell_mesh.py blob --apk /path/to/XRShell.apk --out out
```

Install `out/hand_mesh_fb.bin` using the steps above.

## Disclaimer

`XRShell.apk` and its mesh are the property of PICO. This file is redistributed
solely to make an interoperability feature of an unmodified third-party
application work, with no affiliation with or endorsement by PICO, and it will
be removed on request. If you prefer not to use it, delete this file and either
generate your own copy from your device with `tools/mesh/xrshell_mesh.py`, or
run without the hand mesh.
