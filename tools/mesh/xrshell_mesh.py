#!/usr/bin/env python3
"""Convert the PICO system hand mesh (XRShell.apk) into XR_FB_hand_tracking_mesh data.

Source: assets/mesh/HAND_{L,R}_OpenXR.MESH/ inside /system/priv-app/XRShell/XRShell.apk.
The mesh is a PICO system resource: generated output stays outside the repo
and is never committed or packaged.

Usage (Python 3 with numpy; matplotlib only for `png`):
    python xrshell_mesh.py inspect [--apk APK]
    python xrshell_mesh.py obj     [--apk APK] [--out DIR]
    python xrshell_mesh.py png     [--apk APK] [--out DIR]
    python xrshell_mesh.py blob    [--apk APK] [--out DIR]
    python xrshell_mesh.py all     [--apk APK] [--out DIR]
Every command runs the self-check first and exits non-zero if it fails.

Source file formats (all little endian)
---------------------------------------
face          u32 ntri, u32 0, u32 idx[3*ntri]           (idx == 0..3*ntri-1, no sharing)
normal        u32 nv, f32x3[nv]
uv_0          u32 nv, f32x2[nv]
weight        u32 nv, nv x 37 B: f32x3 pos(m), u8 n(1..4), 4 x {u16 bone, f32 w}
              only the first n influences are valid; the rest hold garbage.
skeleton/skeleton  (5321 B)
    +0   u32  bone_count (26)
    +4   u32  HiID, u32 LoID  (== skeleton.xml <id HiID LoID>)
    +12  u8   1                (probably skeleton.xml beforeSkelPose="true")
    +13  bone_count x 204 B record:
         +0    char16 name[32]        UTF-16LE, NUL padded (tail may hold junk)
         +64   i32  parent            file bone index, -1 = root
         +68   u32  parent_id         bone id of the parent (root: 0x170cbe28)
         +72   u32  bone_id
         +76   f32  bind[16]          row-major 4x4, row-vector convention:
                                      p_model_m = [p_local_cm, 1] @ bind
                                      rows 0..2 = local x/y/z axes * 0.01, row 3 = joint pos (m)
         +140  f32  inv_bind[16]      inverse of bind (x100 rotation, cm), bind @ inv_bind == I
    +5317 f32  1.0                    trailer (skeleton.xml scale?)
Bone local frame: +Z points at the child (toward the finger tip), +Y = back of the hand.

Conversion
----------
Model space: fingers along -Y, back of hand +Z, left thumb -X / right thumb +X
(the two hands are mirror images in X; joint frames are converted so they are NOT mirrored).
Joint frame (OpenXR XR_EXT_hand_tracking): X = -local_x, Y = local_y, Z = -local_z
(180 deg about local Y) -> -Z toward the tip, +Y back of hand, right handed.
Global rotation G (same for both hands): (x, y, z) -> (-x, z, y), i.e. 180 deg about
(0,1,1)/sqrt(2) (= Rx(-90) then Ry(180)). After G the wrist bind pose is identity:
fingers -Z, back +Y, left thumb +X, right thumb -X. Units: metres.
Winding: output triangles are counter-clockwise when seen from the side the vertex
normal points to (FB/Quest, glTF/GL convention). VD reverses the whole index array
(-> clockwise) for Xenko; Godot swaps per triangle for the same reason.

Blob layout (hand_mesh_fb.bin), little endian, every array 16-byte aligned
------------------------------------------------------------------------
FileHeader (32 B)
    +0  char  magic[4] = "VDHM"
    +4  u32   version = 1
    +8  u32   hand_count = 2
    +12 u32   file_size
    +16 u32   crc32 (zlib) of bytes [32, file_size)
    +20 u32   hand_header_size = 64
    +24 u32   reserved[2] = 0
HandHeader[hand_count] (64 B each, at 32 + i*64); offsets are from the start of the file
    +0  u32 hand             XrHandEXT: 1 = left, 2 = right
    +4  u32 joint_count      26
    +8  u32 vertex_count
    +12 u32 index_count      3 * triangles
    +16 u32 off_joint_bind_poses    XrPosef[joint_count]  (qx,qy,qz,qw, px,py,pz: 28 B)
    +20 u32 off_joint_radii         f32[joint_count]
    +24 u32 off_joint_parents       i32[joint_count] XrHandJointEXT; root = 0x7FFFFFFF
    +28 u32 off_vertex_positions    f32x3[vertex_count]
    +32 u32 off_vertex_normals      f32x3[vertex_count]
    +36 u32 off_vertex_uvs          f32x2[vertex_count]
    +40 u32 off_vertex_blend_indices  i16x4[vertex_count] (XrVector4sFB), z = w = 0
    +44 u32 off_vertex_blend_weights  f32x4[vertex_count] (XrVector4f), z = w = 0
    +48 u32 off_indices             i16[index_count]
    +52 u32 reserved[3] = 0
Joint arrays are in XrHandJointEXT order (0 = PALM ... 25 = LITTLE_TIP); blend indices
are XrHandJointEXT values. Each array matches the C layout of the corresponding
XrHandTrackingMeshFB member, so the layer can memcpy it into the app's buffers.
"""

import argparse
import os
import struct
import sys
import zipfile
import zlib

import numpy as np

DEFAULT_APK = os.path.join(os.environ.get("TEMP", ""), "opencode", "meshprobe", "XRShell.apk")
DEFAULT_OUT = os.path.join(os.environ.get("LOCALAPPDATA", ""), "vd-picoenhance", "hand-mesh")

XR_JOINTS = [
    "palm", "wrist",
    "thumb_metacarpal", "thumb_proximal", "thumb_distal", "thumb_tip",
    "index_metacarpal", "index_proximal", "index_intermediate", "index_distal", "index_tip",
    "middle_metacarpal", "middle_proximal", "middle_intermediate", "middle_distal", "middle_tip",
    "ring_metacarpal", "ring_proximal", "ring_intermediate", "ring_distal", "ring_tip",
    "little_metacarpal", "little_proximal", "little_intermediate", "little_distal", "little_tip",
]
# Canonical OpenXR hierarchy (parent of each XrHandJointEXT).
XR_ROOT = 0x7FFFFFFF  # XR_HAND_JOINT_MAX_ENUM_EXT; ovrport treats >= 26 as root
XR_PARENTS = [1, XR_ROOT, 1, 2, 3, 4] + sum(([1, b, b + 1, b + 2, b + 3] for b in (6, 11, 16, 21)), [])
# File bone i -> XrHandJointEXT (derived from names; verified in self_check).
FILE_TO_XR = [1, 6, 21, 16, 17, 11, 12, 22, 7, 2, 3, 4, 23, 8, 9, 13, 14, 18, 19, 24, 0, 10, 5, 25, 20, 15]

# Model space -> output space, (x, y, z) -> (-x, z, y).
G = np.array([[-1.0, 0, 0], [0, 0, 1.0], [0, 1.0, 0]])
# Bone local (cm) -> OpenXR joint frame (m): 180 deg about local Y, x0.01.
LOCAL_TO_JOINT = np.diag([-1.0, 1.0, -1.0])

WEIGHT_DTYPE = np.dtype([("p", "<f4", 3), ("n", "u1"), ("b", [("i", "<u2"), ("w", "<f4")], 4)])


# --------------------------------------------------------------------------- parsing

def load_hand(zf, side):
    base = f"assets/mesh/HAND_{side}_OpenXR.MESH/"
    face = zf.read(base + "face")
    ntri, zero = struct.unpack_from("<II", face)
    idx = np.frombuffer(face, "<u4", offset=8).astype(np.int64)
    normal = zf.read(base + "normal")
    nv = struct.unpack_from("<I", normal)[0]
    nrm = np.frombuffer(normal, "<f4", offset=4).reshape(-1, 3).astype(np.float64)
    uvb = zf.read(base + "uv_0")
    uv = np.frombuffer(uvb, "<f4", offset=4).reshape(-1, 2).astype(np.float64)
    wb = zf.read(base + "weight")
    rec = np.frombuffer(wb, WEIGHT_DTYPE, count=struct.unpack_from("<I", wb)[0], offset=4)
    sk = zf.read(base + "skeleton/skeleton")
    count, hi_id, lo_id = struct.unpack_from("<III", sk)
    bones = []
    for k in range(count):
        r = sk[13 + 204 * k: 13 + 204 * (k + 1)]
        raw = r[:64]
        name = raw[: next((i for i in range(0, 64, 2) if raw[i:i + 2] == b"\0\0"), 64)].decode("utf-16le")
        parent, pid, bid = struct.unpack_from("<iII", r, 64)
        bind = np.frombuffer(r, "<f4", 16, 76).reshape(4, 4).astype(np.float64)
        inv = np.frombuffer(r, "<f4", 16, 140).reshape(4, 4).astype(np.float64)
        bones.append(dict(name=name, parent=parent, parent_id=pid, id=bid, bind=bind, inv=inv))
    trailer = sk[13 + 204 * count:]
    xml = zf.read(base + "skeleton/skeleton.xml").decode("utf-16")
    return dict(side=side, ntri=ntri, face_zero=zero, idx=idx, nv=nv, nrm=nrm, uv=uv, rec=rec,
                bones=bones, sk_len=len(sk), sk_flag=sk[12], hi_id=hi_id, lo_id=lo_id,
                trailer=trailer, xml=xml, sizes=(len(face), len(normal), len(uvb), len(wb)))


def mat_to_quat(m):
    """3x3 rotation (columns = axes) -> (x, y, z, w), w >= 0."""
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    q /= np.linalg.norm(q)
    return -q if q[3] < 0 else q


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


# --------------------------------------------------------------------------- conversion

def convert(h):
    """Return dict with arrays in XR joint order / deduplicated vertex order (output space)."""
    rec = h["rec"]
    # Deduplicate on the full source record + normal + uv, keep first-occurrence order.
    key = np.hstack([rec["p"], h["nrm"], h["uv"], rec["n"][:, None].astype(np.float64),
                     rec["b"]["i"].astype(np.float64), rec["b"]["w"].astype(np.float64)])
    key = np.ascontiguousarray(key)
    _, first, inverse = np.unique(key.view(np.dtype((np.void, key.dtype.itemsize * key.shape[1]))).ravel(),
                                  return_index=True, return_inverse=True)
    order = np.argsort(first)
    remap = np.empty(len(order), np.int64)
    remap[order] = np.arange(len(order))
    src = first[order]
    indices = remap[inverse.ravel()][h["idx"]]

    pos = rec["p"][src].astype(np.float64) @ G.T
    nrm = h["nrm"][src] @ G.T
    uv = h["uv"][src]

    # Blend: first n influences, top-2 by weight, normalised, file bone -> XR joint.
    n = rec["n"][src].astype(np.int64)
    bi = rec["b"]["i"][src].astype(np.int64)
    bw = rec["b"]["w"][src].astype(np.float64)
    valid = np.arange(4)[None, :] < n[:, None]
    bw_valid = np.where(valid, bw, -1.0)
    top = np.argsort(-bw_valid, axis=1, kind="stable")[:, :2]
    rows = np.arange(len(src))[:, None]
    w2 = np.where(bw_valid[rows, top] > 0, bw_valid[rows, top], 0.0)
    w2 = w2 / w2.sum(1, keepdims=True)
    i2 = np.where(w2 > 0, np.array(FILE_TO_XR)[bi[rows, top]], 0)
    blend_idx = np.zeros((len(src), 4), np.int16)
    blend_w = np.zeros((len(src), 4), np.float32)
    blend_idx[:, :2] = i2
    blend_w[:, :2] = w2
    dropped = np.where(valid, bw, 0).sum(1) - np.where(valid, bw, 0)[rows, top].sum(1)

    # Joints.
    xr_to_file = [FILE_TO_XR.index(j) for j in range(26)]
    rot = np.zeros((26, 3, 3))
    tr = np.zeros((26, 3))
    quat = np.zeros((26, 4))
    parents = np.zeros(26, np.int64)
    for j in range(26):
        b = h["bones"][xr_to_file[j]]
        axes = b["bind"][:3, :3] / 0.01          # rows = local axes in model space
        cols = (LOCAL_TO_JOINT @ axes).T         # columns = joint X/Y/Z in model space
        rot[j] = G @ cols
        tr[j] = G @ b["bind"][3, :3]
        quat[j] = mat_to_quat(rot[j])
        parents[j] = XR_ROOT if b["parent"] < 0 else FILE_TO_XR[b["parent"]]
    radii = estimate_radii(pos, rot, tr, parents)
    return dict(pos=pos, nrm=nrm, uv=uv, indices=indices, src=src, blend_idx=blend_idx,
                blend_w=blend_w, dropped=dropped, rot=rot, tr=tr, quat=quat, parents=parents,
                radii=radii, xr_to_file=xr_to_file)


def estimate_radii(pos, rot, tr, parents):
    """Half the palm<->back thickness (along joint +Y) of the mesh around each joint.

    Neighbourhood: |dz| < 4 mm and |dx| < 4 mm in the joint frame (widened to 8 mm if
    fewer than 6 vertices). Tip joints sit on the skin, so their sample is taken 4 mm
    back along the finger (joint +Z)."""
    radii = np.zeros(26)
    for j in range(26):
        centre = tr[j] + (rot[j][:, 2] * 0.004 if XR_JOINTS[j].endswith("_tip") else 0)
        local = (pos - centre) @ rot[j]
        for win in (0.004, 0.008, 0.012):
            m = (np.abs(local[:, 2]) < win) & (np.abs(local[:, 0]) < win)
            if m.sum() >= 6:
                break
        radii[j] = (local[m, 1].max() - local[m, 1].min()) / 2
    return radii


# --------------------------------------------------------------------------- checks

def self_check(h, c, verbose=True):
    errs = []

    def need(cond, msg):
        if not cond:
            errs.append(msg)

    side = h["side"]
    prefix = "left_" if side == "L" else "right_"
    need(len(h["bones"]) == 26, "bone count != 26")
    need(h["sizes"][0] == 8 + 12 * h["ntri"] and h["face_zero"] == 0, "face size/header")
    need(len(h["idx"]) == 3 * h["ntri"] and (h["idx"] == np.arange(len(h["idx"]))).all(), "face not sequential")
    need(h["sizes"][1] == 4 + 12 * h["nv"] and h["sizes"][2] == 4 + 8 * h["nv"]
         and h["sizes"][3] == 4 + 37 * h["nv"], "per-vertex file sizes")
    need(h["sk_len"] == 13 + 204 * 26 + 4 and h["trailer"] == struct.pack("<f", 1.0), "skeleton size/trailer")
    need(f"{h['lo_id']:#x}" in h["xml"].lower() and f"{h['hi_id']:#x}" in h["xml"].lower(), "skeleton id vs xml")
    for i, b in enumerate(h["bones"]):
        need(b["name"].startswith(prefix), f"bone {i} name {b['name']}")
        need(b["name"][len(prefix):] == XR_JOINTS[FILE_TO_XR[i]], f"FILE_TO_XR[{i}] vs name {b['name']}")
        need(np.abs(b["bind"] @ b["inv"] - np.eye(4)).max() < 1e-5, f"bind*inv != I for {b['name']}")
        need(b["parent"] == -1 or h["bones"][b["parent"]]["id"] == b["parent_id"], f"parent id {b['name']}")
        need(f'name="{b["name"]}"' in h["xml"], f"{b['name']} missing in xml")
    need(list(c["parents"]) == XR_PARENTS, "joint parents differ from OpenXR hierarchy")
    rec = h["rec"]
    valid = np.arange(4)[None, :] < rec["n"][:, None]
    need(((rec["n"] >= 1) & (rec["n"] <= 4)).all(), "influence count out of 1..4")
    need(np.abs(np.where(valid, rec["b"]["w"], 0).sum(1) - 1).max() < 1e-4, "source weights sum != 1")
    need(np.allclose(np.linalg.norm(h["nrm"], axis=1), 1, atol=1e-4), "normals not unit")

    nv, ni = len(c["pos"]), len(c["indices"])
    need(nv == 1361 and ni == 3 * 2718, f"unexpected counts {nv} v / {ni // 3} tri")
    need(ni % 3 == 0, "index count not a triangle list")
    need(c["indices"].min() >= 0 and c["indices"].max() < nv and nv <= 32767, "index out of range / int16")
    tri = c["indices"].reshape(-1, 3)
    need((tri[:, 0] != tri[:, 1]).all() and (tri[:, 1] != tri[:, 2]).all() and (tri[:, 0] != tri[:, 2]).all(),
         "degenerate triangle")
    need(np.abs(c["blend_w"].sum(1) - 1).max() < 1e-5, "blend weights sum != 1")
    need((c["blend_idx"] >= 0).all() and (c["blend_idx"] < 26).all(), "blend index range")
    for name in ("pos", "nrm", "uv", "quat", "tr", "radii", "blend_w"):
        need(np.isfinite(c[name]).all(), f"NaN/inf in {name}")
    ext = c["pos"].max(0) - c["pos"].min(0)
    need(0.15 < ext.max() < 0.25, f"bounds {ext}")
    need(np.abs(np.linalg.norm(c["quat"], axis=1) - 1).max() < 1e-6, "quaternion not unit")
    need(np.abs(np.array([quat_to_mat(q) for q in c["quat"]]) - c["rot"]).max() < 1e-5, "quat != rot")
    need(np.abs(c["rot"][1] - np.eye(3)).max() < 1e-5 and np.abs(c["tr"][1]).max() < 1e-6, "wrist bind != identity")
    need(((c["radii"] > 0.003) & (c["radii"] < 0.035)).all(), f"radii {c['radii']}")

    # OpenXR joint convention: -Z toward the child, thumb on +X (left) / -X (right) of the wrist.
    for j in range(26):
        kids = [k for k in range(26) if c["parents"][k] == j]
        if kids and j not in (0, 1):
            d = c["tr"][kids[0]] - c["tr"][j]
            need((d / np.linalg.norm(d)) @ -c["rot"][j][:, 2] > 0.99, f"-Z of {XR_JOINTS[j]} not toward child")
    need(c["tr"][5][2] < 0 and c["tr"][15][2] < -0.15, "fingers not along -Z")
    thumb_x = c["tr"][5][0]
    need(thumb_x > 0 if side == "L" else thumb_x < 0, "thumb on wrong side (hand mirrored?)")
    need(c["tr"][5] @ c["rot"][0][:, 1] < 0, "thumb tip not on palm side of palm +Y")

    # Bind pose vs vertices: inv(XrPosef) applied to output vertices == file local coordinates.
    top = c["blend_idx"][:, 0]
    loc_pose = np.einsum("nij,ni->nj", c["rot"][top], c["pos"] - c["tr"][top])
    file_b = np.array([h["bones"][c["xr_to_file"][j]]["inv"] for j in top])
    raw = np.hstack([h["rec"]["p"][c["src"]].astype(np.float64), np.ones((nv, 1))])
    loc_file = np.einsum("ni,nij->nj", raw, file_b)[:, :3] @ LOCAL_TO_JOINT * 0.01
    bind_err = np.abs(loc_pose - loc_file).max()
    need(bind_err < 1e-5, f"bind pose / vertex mismatch {bind_err}")

    # Winding: CCW face normal agrees with the vertex normals.
    p = c["pos"][tri]
    fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    agree = np.einsum("ni,ni->n", fn, c["nrm"][tri].sum(1)) > 0
    need(agree.mean() > 0.98, f"CCW winding agreement {agree.mean():.3f}")

    if verbose:
        print(f"[{side}] self-check: {'OK' if not errs else 'FAIL'}  vertices={nv} triangles={ni // 3} "
              f"extent(m)={np.round(ext, 4)} ccw_agree={agree.mean():.4f} bind_err={bind_err:.1e} "
              f"dropped_weight(max/mean)={c['dropped'].max():.3f}/{c['dropped'].mean():.4f}")
        for e in errs:
            print("   FAIL:", e)
    return not errs


# --------------------------------------------------------------------------- commands

def cmd_inspect(hands, conv):
    np.set_printoptions(suppress=True, precision=4, linewidth=160)
    for h in hands:
        c = conv[h["side"]]
        rec = h["rec"]
        print(f"\n=== HAND_{h['side']}  ntri={h['ntri']} nv={h['nv']} skeleton id Hi={h['hi_id']:#010x} "
              f"Lo={h['lo_id']:#010x} flag={h['sk_flag']} trailer={struct.unpack('<f', h['trailer'])[0]}")
        print("raw pos min/max:", rec["p"].min(0), rec["p"].max(0))
        print("uv min/max:", h["uv"].min(0), h["uv"].max(0))
        print("influences per vertex (n=1..4):", np.bincount(rec["n"], minlength=5)[1:])
        used = np.unique(np.concatenate([rec["b"]["i"][rec["n"] > k, k] for k in range(4)]))
        print("file bones used by skinning:", used.tolist())
        print(f"dedup: {h['nv']} -> {len(c['pos'])} vertices")
        print("file bones (bind row3 = joint position m, dist = nearest vertex):")
        for i, b in enumerate(h["bones"]):
            pj = b["bind"][3, :3]
            d = np.linalg.norm(rec["p"] - pj, axis=1).min()
            err = np.abs(b["bind"] @ b["inv"] - np.eye(4)).max()
            ax = b["bind"][:3, :3] / 0.01
            print(f"  {i:2d} {b['name']:26s} parent={b['parent']:2d} id={b['id']:08x} -> XR {FILE_TO_XR[i]:2d} "
                  f"pos={np.round(pj, 4)} near={d * 1000:5.1f}mm |bind*inv-I|={err:.0e} "
                  f"localZ={np.round(ax[2], 2)} localY={np.round(ax[1], 2)}")
        print("XR joints (output space): XrPosef quat(xyzw) / position(m) / radius(m) / parent")
        for j in range(26):
            par = "ROOT" if c["parents"][j] == XR_ROOT else c["parents"][j]
            print(f"  {j:2d} {XR_JOINTS[j]:20s} q={np.round(c['quat'][j], 4)} p={np.round(c['tr'][j], 4)} "
                  f"r={c['radii'][j]:.4f} parent={par}")


def write_obj(path_mesh, path_skel, c, side):
    with open(path_mesh, "w", encoding="ascii", newline="\n") as f:
        f.write(f"# PICO XRShell HAND_{side}, OpenXR wrist space (m), CCW front faces\n")
        for v in c["pos"]:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        for t in c["uv"]:
            f.write(f"vt {t[0]:.6f} {t[1]:.6f}\n")
        for n in c["nrm"]:
            f.write(f"vn {n[0]:.6f} {n[1]:.6f} {n[2]:.6f}\n")
        for a, b, d in c["indices"].reshape(-1, 3) + 1:
            f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {d}/{d}/{d}\n")
    with open(path_skel, "w", encoding="ascii", newline="\n") as f:
        f.write(f"# HAND_{side} joints (v 1..26 = XrHandJointEXT 0..25), parent lines, then per-joint "
                "1 cm axis lines: +X, +Y (back of hand), -Z (toward tip)\n")
        for j, t in enumerate(c["tr"]):
            f.write(f"v {t[0]:.6f} {t[1]:.6f} {t[2]:.6f}  # {XR_JOINTS[j]}\n")
        for j, p in enumerate(c["parents"]):
            if p != XR_ROOT:
                f.write(f"l {p + 1} {j + 1}\n")
        n = 27
        for j in range(26):
            for axis in (c["rot"][j][:, 0], c["rot"][j][:, 1], -c["rot"][j][:, 2]):
                e = c["tr"][j] + axis * 0.01
                f.write(f"v {e[0]:.6f} {e[1]:.6f} {e[2]:.6f}\nl {j + 1} {n}\n")
                n += 1


def write_png(path, h, c):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    raw_pos = h["rec"]["p"][c["src"]]
    raw_joint = np.array([h["bones"][c["xr_to_file"][j]]["bind"][3, :3] for j in range(26)])
    views = [
        ("source file space: X right, Y up (front)", raw_pos, raw_joint, None, (0, 1), ("X", "Y")),
        ("OpenXR wrist space, top: X right, -Z up", c["pos"], c["tr"], c["rot"], (0, 2), ("X", "Z")),
        ("OpenXR wrist space, side: -Z right, Y up", c["pos"], c["tr"], c["rot"], (2, 1), ("Z", "Y")),
        ("OpenXR wrist space, front: X right, Y up", c["pos"], c["tr"], c["rot"], (0, 1), ("X", "Y")),
    ]
    fig, axs = plt.subplots(2, 2, figsize=(14, 14))
    for ax, (title, pv, pj, rot, (a, b), (la, lb)) in zip(axs.ravel(), views):
        flip_a = -1 if la == "Z" else 1
        flip_b = -1 if lb == "Z" else 1
        ax.scatter(pv[:, a] * flip_a, pv[:, b] * flip_b, s=1, c="0.75")
        for j in range(26):
            p = c["parents"][j]
            if p != XR_ROOT:
                ax.plot([pj[p, a] * flip_a, pj[j, a] * flip_a], [pj[p, b] * flip_b, pj[j, b] * flip_b], "k-", lw=1)
            if rot is not None:
                for vec, col in ((rot[j][:, 0], "r"), (rot[j][:, 1], "g"), (-rot[j][:, 2], "b")):
                    e = pj[j] + vec * 0.012
                    ax.plot([pj[j, a] * flip_a, e[a] * flip_a], [pj[j, b] * flip_b, e[b] * flip_b], col + "-", lw=1.2)
            ax.annotate(str(j), (pj[j, a] * flip_a, pj[j, b] * flip_b), fontsize=7, color="purple")
        ax.scatter(pj[:, a] * flip_a, pj[:, b] * flip_b, s=12, c="purple", zorder=3)
        ax.set_title(title, fontsize=10)
        ax.set_xlabel(("-" if flip_a < 0 else "+") + la + " (m)")
        ax.set_ylabel(("-" if flip_b < 0 else "+") + lb + " (m)")
        ax.set_aspect("equal")
        ax.grid(True, lw=0.3)
    fig.suptitle(f"PICO XRShell HAND_{h['side']}: joints = XrHandJointEXT index; axes red +X, green +Y "
                 "(back of hand), blue -Z (toward tip); 12 mm", fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def build_blob(conv):
    def align(buf):
        buf.extend(b"\0" * (-len(buf) % 16))

    hands = [("L", 1), ("R", 2)]
    body = bytearray(b"\0" * (32 + 64 * len(hands)))
    headers = []
    for side, xr_hand in hands:
        c = conv[side]
        arrays = [
            np.hstack([c["quat"], c["tr"]]).astype("<f4"),
            c["radii"].astype("<f4"),
            c["parents"].astype("<i4"),
            c["pos"].astype("<f4"),
            c["nrm"].astype("<f4"),
            c["uv"].astype("<f4"),
            c["blend_idx"].astype("<i2"),
            c["blend_w"].astype("<f4"),
            c["indices"].astype("<i2"),
        ]
        offs = []
        for arr in arrays:
            align(body)
            offs.append(len(body))
            body.extend(np.ascontiguousarray(arr).tobytes())
        headers.append(struct.pack("<4I9I3I", xr_hand, 26, len(c["pos"]), len(c["indices"]), *offs, 0, 0, 0))
    align(body)
    for i, hh in enumerate(headers):
        body[32 + 64 * i: 32 + 64 * (i + 1)] = hh
    crc = zlib.crc32(bytes(body[32:])) & 0xFFFFFFFF
    body[0:32] = struct.pack("<4s5I2I", b"VDHM", 1, len(hands), len(body), crc, 64, 0, 0)
    return bytes(body)


def verify_blob(blob, conv):
    magic, ver, nh, size, crc, hsz = struct.unpack_from("<4s5I", blob)
    ok = magic == b"VDHM" and ver == 1 and nh == 2 and size == len(blob) and hsz == 64
    ok &= crc == zlib.crc32(blob[32:]) & 0xFFFFFFFF
    for i, side in enumerate("LR"):
        f = struct.unpack_from("<16I", blob, 32 + 64 * i)
        hand, jc, vc, ic = f[:4]
        o = f[4:13]
        c = conv[side]
        ok &= hand == i + 1 and jc == 26 and all(x % 16 == 0 for x in o)
        rd = lambda k, dt, n, w: np.frombuffer(blob, dt, n * w, o[k]).reshape(n, w) if w > 1 else np.frombuffer(blob, dt, n, o[k])
        ok &= np.allclose(rd(0, "<f4", jc, 7)[:, :4], c["quat"], atol=1e-6)
        ok &= (rd(2, "<i4", jc, 1) == c["parents"]).all()
        ok &= np.allclose(rd(3, "<f4", vc, 3), c["pos"], atol=1e-6)
        ok &= (rd(6, "<i2", vc, 4) == c["blend_idx"]).all()
        ok &= (rd(8, "<i2", ic, 1) == c["indices"]).all() and o[8] + 2 * ic <= size
    return bool(ok)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=["inspect", "obj", "png", "blob", "all"])
    ap.add_argument("--apk", default=DEFAULT_APK)
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()

    with zipfile.ZipFile(args.apk) as zf:
        hands = [load_hand(zf, s) for s in "LR"]
    conv = {h["side"]: convert(h) for h in hands}
    ok = all(self_check(h, conv[h["side"]]) for h in hands)
    if args.command == "inspect":
        cmd_inspect(hands, conv)
        return 0 if ok else 1
    if not ok:
        print("self-check failed; nothing written", file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    if args.command in ("obj", "all"):
        for h in hands:
            s = h["side"]
            write_obj(os.path.join(args.out, f"hand_{s}.obj"), os.path.join(args.out, f"hand_{s}_skeleton.obj"),
                      conv[s], s)
        print("obj ->", args.out)
    if args.command in ("png", "all"):
        for h in hands:
            write_png(os.path.join(args.out, f"hand_{h['side']}.png"), h, conv[h["side"]])
        print("png ->", args.out)
    if args.command in ("blob", "all"):
        blob = build_blob(conv)
        if not verify_blob(blob, conv):
            print("blob read-back failed", file=sys.stderr)
            return 1
        path = os.path.join(args.out, "hand_mesh_fb.bin")
        with open(path, "wb") as f:
            f.write(blob)
        print(f"blob -> {path} ({len(blob)} B, crc32 {struct.unpack_from('<I', blob, 16)[0]:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
