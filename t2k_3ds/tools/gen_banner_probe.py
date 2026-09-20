#!/usr/bin/env python3
"""
gen_banner_probe.py -- the SIMPLEST thing pycgfx can produce, as a banner.

    python3 t2k_3ds/tools/gen_banner_probe.py --out <dir> [--level N]

A spinning cube: 12 triangles, vertex colours, no texture, the same two-node
seat/spin rig and the same animation shape the real banner uses. About 10 KB
against the real model's 424.

WHY IT EXISTS. The HOME menu does not report a bad banner, it FREEZES -- so the
only way to learn what it will not accept is to remove things and reinstall, and
that is a bisect that has to start from a known-good end. We had been shaving
the real model DOWN (drop the animation, shorten the loop) and it kept freezing,
which tells us what is NOT the cause but never what is. This starts from the
other end: if a cube freezes too, then nothing this pipeline emits works on this
console and the answer is to stop and ship a still. If the cube is fine, then
complexity can be added back -- texture, triangle count, node depth -- until it
breaks, and the step that breaks it IS the answer.

It deliberately shares gen_banner_level.py's rig rather than inventing one, so a
pass here really does clear the rig: same node graph (a placed seat carrying the
Rx(-90) conjugation, a child mesh node taking the Z roll), same key layout, same
material shape, same glTF writer.
"""

import argparse
import importlib.util
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "genlevel", os.path.join(HERE, "gen_banner_level.py"))
G = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(G)


def build_cube(half, band):
    """A cube of side 2*half, one solid-colour face pair per band hue so the
    thing is visibly the game's palette and visibly turning."""
    m = G.Mesh("probe_cube", "probe")
    h0, h1, s0, s1 = band
    # (normal axis, sign) for the six faces
    faces = [((1, 0, 0), 1), ((1, 0, 0), -1), ((0, 1, 0), 1),
             ((0, 1, 0), -1), ((0, 0, 1), 1), ((0, 0, 1), -1)]
    for i, (axis, sgn) in enumerate(faces):
        t = i / 5.0
        c = G.hsv(h0 + (h1 - h0) * t, s0 + (s1 - s0) * t, 1.0)
        # two in-plane axes
        u = (0, 1, 0) if axis[0] else ((0, 0, 1) if axis[1] else (1, 0, 0))
        v = (0, 0, 1) if axis[0] else ((1, 0, 0) if axis[1] else (0, 1, 0))
        n = [a * sgn for a in axis]
        cen = [a * sgn * half for a in axis]
        pts = []
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            pts.append([cen[k] + u[k] * su * half + v[k] * sv * half
                        for k in range(3)])
        m.quad(pts[0], pts[1], pts[2], pts[3], (c, c, c, c), n=n)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--level", type=int, default=15,
                    help="only picks the colour band, so the probe looks like "
                         "it belongs to the same game")
    ap.add_argument("--frames", type=float, default=4.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    idx = args.level - 1
    band = G.read_band((idx // G.WEB_BAND_LEVELS) % G.WEB_BATCH_COUNT)

    # Sized and seated the same way the real banner is, so the probe occupies
    # the frame rather than sitting in a corner: a cube's own silhouette is its
    # bounding circle, so a ring of 4 corners is the whole outline.
    ring = [(math.cos(a * math.pi / 2 + math.pi / 4) * 1.0,
             math.sin(a * math.pi / 2 + math.pi / 4) * 1.0) for a in range(4)]
    radius, seat = G.solve_framing(ring)
    cube = build_cube(radius * 0.62, band)

    # tube space -> mesh space, exactly as build_tube's last pass does, so the
    # Z roll conjugates into the same Y yaw
    cube.pos = [(x, -z, y) for (x, y, z) in cube.pos]
    cube.nrm = [(x, -z, y) for (x, y, z) in cube.nrm]

    materials = [{"name": "probe",
                  "pbrMetallicRoughness": {
                      "baseColorFactor": [0.30, 0.30, 0.34, 1.0],
                      "roughnessFactor": 1.0},
                  "emissiveFactor": [0.92, 0.92, 0.96], "doubleSided": False}]

    parent = G.quat_axis((1, 0, 0), G.CAM_PITCH - math.pi * 0.5)
    nodes = [
        {"name": "Root", "children": [1]},
        {"name": "TubeSeat", "translation": [seat[0], seat[1], seat[2]],
         "rotation": parent, "children": [2]},
        {"name": "TubeSpin", "mesh": 0},
    ]
    times = [args.frames * k / 4.0 for k in range(5)]
    quats = [G.quat_axis((0, 0, 1), G.SPIN_SIGN * 2.0 * math.pi * k / 4.0)
             for k in range(5)]

    G.write_gltf(os.path.join(args.out, "banner.gltf"), [[cube]], materials,
                 [], nodes, [[(2, times, quats)]])
    print("gen_banner_probe: cube, %d triangles, no texture, %.1f s loop "
          "(%.0f frames) -> %s"
          % (len(cube.idx) // 3, args.frames, args.frames * 60,
             os.path.join(args.out, "banner.gltf")))


if __name__ == "__main__":
    main()
