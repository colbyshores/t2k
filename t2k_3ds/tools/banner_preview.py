#!/usr/bin/env python3
"""
banner_preview.py -- render gen_banner_model.py's glTF through the HOME menu's
OWN camera, so the banner can be LOOKED AT before it is packed into a CIA.

This exists because the banner is otherwise unverifiable on this machine:
Mandarine does not render HOME-menu banners, and a CIA only shows its banner
once it is installed on a console. "Validation != build success" (DOCTRINE.md's
3DS validation loop) needs SOMETHING to look at, and this is it.

WHAT IT APPROXIMATES, and where it is only an approximation. Camera, aspect,
projection, geometry and animation timing are exact -- they are read from the
same glTF the converter reads, through the camera pycgfx records for the HOME
menu. The SHADING is a stand-in for the PICA's fixed-function pipeline: vertex
colour x texture, scaled by (emission + diffuse * N.L) against the one
directional light pycgfx emits, which is what the material's combiner stack
works out to. Blending, mipmaps and the menu's own backdrop are not modelled.
So: trust it for composition, silhouette, spin and framing; do not trust it for
exact brightness.

    python3 t2k_3ds/tools/banner_preview.py <gltfdir> <out.png> [--t 0.25]
    python3 t2k_3ds/tools/banner_preview.py <gltfdir> <out.png> --strip 6
"""

import argparse
import json
import math
import os
import struct

import numpy as np
from PIL import Image

# pycgfx's banner-camera.gltf: the HOME menu's fixed camera.
CAM_POS = (0.0, 1.0, 44.786)
CAM_YFOV = 0.523599
CAM_ASPECT = 1.66666666667
CAM_ZNEAR = 26.5


def load(dirpath):
    doc = json.load(open(os.path.join(dirpath, "banner.gltf")))
    buf = open(os.path.join(dirpath, doc["buffers"][0]["uri"]), "rb").read()

    def acc(i):
        a = doc["accessors"][i]
        v = doc["bufferViews"][a["bufferView"]]
        comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        fmt, size = (("f", 4) if a["componentType"] == 5126 else ("H", 2))
        off = v.get("byteOffset", 0)
        n = a["count"] * comps
        raw = struct.unpack_from("<%d%s" % (n, fmt), buf, off)
        return np.array(raw, dtype=np.float32 if fmt == "f" else np.uint32
                        ).reshape(a["count"], comps)
    return doc, acc


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float64)


def slerp(a, b, t):
    a = np.array(a, dtype=np.float64)
    b = np.array(b, dtype=np.float64)
    d = float(np.dot(a, b))
    if d < 0:
        b, d = -b, -d
    if d > 0.9995:
        r = a + t * (b - a)
        return r / np.linalg.norm(r)
    th = math.acos(max(-1.0, min(1.0, d)))
    s = math.sin(th)
    return (math.sin((1 - t) * th) / s) * a + (math.sin(t * th) / s) * b


def sample_rotation(doc, acc, node_index, time):
    for anim in doc.get("animations", []):
        for ch in anim["channels"]:
            if ch["target"]["node"] != node_index or ch["target"]["path"] != "rotation":
                continue
            s = anim["samplers"][ch["sampler"]]
            ts = acc(s["input"])[:, 0]
            qs = acc(s["output"])
            t = float(np.clip(time, ts[0], ts[-1]))
            i = int(np.searchsorted(ts, t, side="right")) - 1
            i = max(0, min(i, len(ts) - 2))
            f = (t - ts[i]) / max(1e-9, ts[i + 1] - ts[i])
            return slerp(qs[i], qs[i + 1], f)
    return None


def render(dirpath, time, width, height):
    doc, acc = load(dirpath)
    tex = None
    if "images" in doc:
        tex = np.asarray(Image.open(os.path.join(dirpath, doc["images"][0]["uri"]))
                         .convert("RGB"), dtype=np.float32) / 255.0

    tris = []          # (zdepth, screen pts, colours, uvs, normals, material)
    for ni, node in enumerate(doc["nodes"]):
        if "mesh" not in node:
            continue
        # world transform: walk the (shallow, hand-built) parent chain
        chain, cur = [], ni
        parent = {c: p for p, n in enumerate(doc["nodes"])
                  for c in n.get("children", [])}
        while True:
            chain.append(cur)
            if cur not in parent:
                break
            cur = parent[cur]
        M = np.eye(4)
        for idx in reversed(chain):
            n = doc["nodes"][idx]
            q = sample_rotation(doc, acc, idx, time)
            if q is None:
                q = n.get("rotation")
            L = np.eye(4)
            if q is not None:
                L[:3, :3] = quat_to_mat(q)
            if "translation" in n:
                L[:3, 3] = n["translation"]
            M = M @ L
        mesh = doc["meshes"][node["mesh"]]
        for prim in mesh["primitives"]:
            pos = acc(prim["attributes"]["POSITION"])
            nrm = acc(prim["attributes"]["NORMAL"])
            col = acc(prim["attributes"]["COLOR_0"])
            uv = acc(prim["attributes"]["TEXCOORD_0"])
            idx = acc(prim["indices"])[:, 0].astype(int)
            mat = doc["materials"][prim["material"]]
            wp = (M[:3, :3] @ pos.T).T + M[:3, 3]
            wn = (M[:3, :3] @ nrm.T).T
            for k in range(0, len(idx), 3):
                a, b, c = idx[k], idx[k + 1], idx[k + 2]
                tris.append((wp[[a, b, c]], wn[[a, b, c]], col[[a, b, c]],
                             uv[[a, b, c]], mat))

    # ADDITIVE MATERIALS DRAW LAST AND DO NOT WRITE DEPTH, which is the whole
    # reason this pass exists. A halo rendered the ordinary way REPLACES the hot
    # core it surrounds and the model comes out DARKER than the one without it
    # -- the exact inverse of what the console does, so a preview that ignores
    # `extras.blendMode` does not merely under-report the glow, it inverts it.
    # Sorting them last is what makes the depth test meaningful: they must be
    # tested against the opaque geometry, never overwritten by it.
    tris.sort(key=lambda t: (t[4].get("extras", {}) or {}).get("blendMode") == "ADD")

    img = np.zeros((height, width, 3), dtype=np.float32)
    zbuf = np.full((height, width), 1e30, dtype=np.float32)
    fy = 1.0 / math.tan(CAM_YFOV * 0.5)
    fx = fy / CAM_ASPECT
    light = np.array([0.0, 0.0, 1.0])       # pycgfx's CFLT, direction (0,0,-1)

    for wp, wn, col, uv, mat in tris:
        v = wp - np.array(CAM_POS)
        z = -v[:, 2]
        if np.any(z <= CAM_ZNEAR * 0.05):
            continue
        sx = (v[:, 0] * fx / z * 0.5 + 0.5) * width
        sy = (0.5 - v[:, 1] * fy / z * 0.5) * height
        x0 = max(0, int(np.floor(sx.min()))); x1 = min(width - 1, int(np.ceil(sx.max())))
        y0 = max(0, int(np.floor(sy.min()))); y1 = min(height - 1, int(np.ceil(sy.max())))
        if x1 < x0 or y1 < y0:
            continue
        ax, ay = sx[0], sy[0]; bx, by = sx[1], sy[1]; cx, cy = sx[2], sy[2]
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-9:
            continue
        px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5,
                             np.arange(y0, y1 + 1) + 0.5)
        u = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den
        vv = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den
        w = 1.0 - u - vv
        m = (u >= 0) & (vv >= 0) & (w >= 0)
        if not m.any():
            continue
        zz = u * z[0] + vv * z[1] + w * z[2]
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        m &= zz < sub
        if not m.any():
            continue
        base = (u[..., None] * col[0, :3] + vv[..., None] * col[1, :3] +
                w[..., None] * col[2, :3])
        if tex is not None and "baseColorTexture" in mat.get("pbrMetallicRoughness", {}):
            tu = u * uv[0, 0] + vv * uv[1, 0] + w * uv[2, 0]
            tv = u * uv[0, 1] + vv * uv[1, 1] + w * uv[2, 1]
            ti = np.clip((tv * (tex.shape[0] - 1)).astype(int), 0, tex.shape[0] - 1)
            tj = np.mod((tu * (tex.shape[1] - 1)).astype(int), tex.shape[1])
            base = base * tex[ti, tj]
        n = (u[..., None] * wn[0] + vv[..., None] * wn[1] + w[..., None] * wn[2])
        nl = np.abs((n / (np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9)) @ light)
        em = np.array(mat.get("emissiveFactor", [0, 0, 0]))
        pbr = mat["pbrMetallicRoughness"]
        kd = np.array(pbr.get("baseColorFactor", [1, 1, 1, 1])[:3])
        shade = np.clip(em + kd * nl[..., None], 0.0, 1.0)
        # SPECULAR IS ADDED, IT IS WHITE, AND LEAVING IT OUT HID A REAL BUG.
        # pycgfx emits a third TEV stage that ADDS constant[0] * secondary
        # colour, with constant[0] = 1 - 0.9*roughness. Modelling only emission
        # and diffuse made this preview show a correct pink wordmark and correct
        # coloured haloes while the console rendered both WHITE -- the add
        # swamps any surface dim enough, which is exactly what a glow is.
        rough = pbr.get("roughnessFactor", 1.0)
        spec = (mat.get("extras", {}) or {}).get("specular")
        k0 = float(spec) if spec is not None else (1.0 - 0.9 * rough)
        shin = 4.0 * 200.0 / (1.0 + rough * 100.0)
        outc = np.clip(base * shade + k0 * (nl[..., None] ** shin), 0.0, 1.0)
        dst = img[y0:y1 + 1, x0:x1 + 1]
        if (mat.get("extras", {}) or {}).get("blendMode") == "ADD":
            dst[m] += outc[m]          # One/One; the console clamps, so do we
        else:
            dst[m] = outc[m]
            sub[m] = zz[m]             # only opaque geometry occludes
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("out")
    ap.add_argument("--t", type=float, default=0.0, help="animation time, seconds")
    ap.add_argument("--strip", type=int, default=0,
                    help="render N frames evenly across the loop, side by side")
    ap.add_argument("--w", type=int, default=512)
    args = ap.parse_args()
    h = int(args.w / CAM_ASPECT)
    if args.strip:
        doc = json.load(open(os.path.join(args.dir, "banner.gltf")))
        _, acc = load(args.dir)
        dur = max(float(acc(s["input"])[-1, 0])
                  for a in doc["animations"] for s in a["samplers"])
        sheet = Image.new("RGB", (args.w, h * args.strip))
        for i in range(args.strip):
            sheet.paste(render(args.dir, dur * i / args.strip, args.w, h),
                        (0, i * h))
        sheet.save(args.out)
    else:
        render(args.dir, args.t, args.w, h).save(args.out)
    print("banner_preview: wrote", args.out)


if __name__ == "__main__":
    main()
