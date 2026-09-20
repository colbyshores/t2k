#!/usr/bin/env python3
"""
patch_pycgfx.py -- teach the vendored pycgfx converter about glTF emissiveFactor.

WHY. Upstream pycgfx (skyfloogle/pycgfx, the glTF -> CGFX converter that makes
the HOME-menu banner model) drops `emissiveFactor` entirely: every material it
emits is purely directionally lit by the single scene light it also emits, so
any surface angled away from that light renders BLACK. T2K's banner is emissive
neon on black -- a tube wall seen edge-on has to keep glowing -- so without this
the model reads as a dark grey box that lights up only when it happens to face
the light.

WHAT. Emission is part of the standard PICA fragment-lighting equation (it is
added straight into the fragment primary colour), so honouring it needs exactly
one assignment and touches no combiner, flag or shader. That is the whole patch.

The converter is cloned into tools/.ciatools/ (gitignored) by get_cia_tools.sh,
so the patch lives here as code rather than as a fork. Idempotent.
"""
import sys

MARK = "material.emissiveFactor"

# ---- second patch: the bone RE-SORT breaks animation targeting ---------------
# pycgfx reorders the bone list AFTER building the hierarchy --
# `bones.dict.nodes[2:] = sorted(..., key=sort_key)`, ranking bones by how many
# translucent materials they use -- and then rebuilds joint ids and parent ids
# from the new positions. The animation's target is resolved through that same
# remap, and it comes out pointing at the WRONG BONE: a banner whose spin was
# authored on the tube's own node had the CANM land on an ANCESTOR of the whole
# scene, so the tube span AND the wordmark orbited it.
#
# The sort is a draw-order optimisation for translucency. This model has no
# blended materials at all, so it sorts nothing and only costs correctness.
SORT_MARK = "# pycgfx bone sort disabled"
SORT_ANCHOR = """    cmdl.skeleton.bones.dict.nodes[2:] = sorted(
        cmdl.skeleton.bones.dict.nodes[2:], key=sort_key
    )"""
SORT_REPLACE = """    # pycgfx bone sort disabled by tools/patch_pycgfx.py -- it re-indexed the
    # skeleton after the hierarchy was built and animation targeting followed
    # the stale indices. Nothing here uses blended materials, so the sort this
    # replaces had no work to do.
    pass"""

# ---- third patch: BILLBOARD MODE, the documented way to pin a banner logo ----
# The HOME menu's banner convention (GBAtemp's "How to Make a 3D Banner Like an
# Official Nintendo 3DS Game", and pycgfx's own README) is:
#
#     COMMON            the scene root
#     |-- world         the rotating model root
#     `-- name          an independent BILLBOARD logo, SIBLING of world
#
# Being a sibling is necessary and NOT sufficient. glTF has no field for the
# CGFX billboard mode, so pycgfx leaves every bone at BillboardMode.Off, and a
# plain sibling logo does not stay put -- ours orbited the web. The documented
# fix is to set the logo bone to YAxial (5), which makes the runtime face that
# bone at the camera every frame instead of using its inherited orientation.
# That is what an official banner does, and it is why Star Fox 64 3D's logo
# holds still over a spinning model.
#
# Expressed as glTF `extras` so the generator stays the single source of truth
# rather than the file being hand-edited in ImHex after every build.
BILLBOARD_MARK = "extras.billboardMode"
BILLBOARD_ANCHOR = """        if node.children:
            child_bones = make_bones(gltf, node.children, bone_dict)"""
BILLBOARD_REPLACE = """        # glTF `extras.billboardMode` -> the CGFX bone's BILLBOARD MODE
        # (added by tools/patch_pycgfx.py -- see that file for why).
        if node.extras and "billboardMode" in node.extras:
            bone.billboard_mode = BillboardMode(int(node.extras["billboardMode"]))

        if node.children:
            child_bones = make_bones(gltf, node.children, bone_dict)"""

# ---- fifth patch: THE SPECULAR CONSTANT, which was painting everything white --
# pycgfx derives the specular constant from roughness alone:
#     material_color.constant[0] = 1 - 0.9 * roughnessFactor
# and TEV stage 2 ADDS that times the fragment secondary colour. Specular is
# WHITE, and it is added AFTER the vertex colour has been applied -- so how much
# it matters depends entirely on how bright the surface under it is.
#
# That is what made the wordmark render WHITE instead of pink: at roughness 0.35
# the constant is 0.685, and 0.685 of white added to `#f2baff` clamps to white.
# The tube's strokes survived it only because they are bright enough (~0.8) for
# a 0.235 add to read as a highlight rather than a wash. The additive glow was
# the worst case of all: its own colour is 0.02..0.15, so even the roughness-1.0
# minimum of 0.10 white swamped the hue and every halo came out white.
#
# glTF has no field for it (roughness is the only handle, and it cannot reach
# zero: 1 - 0.9*1.0 = 0.10), so a material may set it directly with
# `extras: {"specular": 0.0}`. A neon glow has no specular highlight -- it is
# not a lit surface, it is light.
SPEC_MARK = 'extras.specular'
SPEC_ANCHOR = """                lut = LutTable.phong(4 * 200 / (1 + roughness_factor * 100))"""
SPEC_REPLACE = """                # glTF `extras.specular` -> the specular constant, overriding
                # the roughness-derived default (tools/patch_pycgfx.py).
                if (material.extras or {}).get("specular") is not None:
                    _s = float(material.extras["specular"])
                    mtob.material_color.constant[0] = ColorFloat(_s, _s, _s, 1)

                lut = LutTable.phong(4 * 200 / (1 + roughness_factor * 100))"""

# ---- fourth patch: ADDITIVE BLEND, so the banner can carry the game's glow ----
# DOCTRINE.md used to say "CGFX has no additive blend". That is true of glTF, NOT
# of CGFX: the MTOB carries a full blend descriptor and pycgfx's own
# BlendFunction enum has One. glTF only offers OPAQUE / MASK / BLEND, so an
# additive material simply cannot be spelled in the source format -- which is
# what the note was really about.
#
# T2K's whole look is "neon-on-black vectors, ADDITIVE glow". Alpha blending
# cannot stand in for it: a translucent halo over black DARKENS the core it
# covers, where an additive one adds light to it. So a material may ask for
# One/One with `extras: {"blendMode": "ADD"}`, and depth WRITE is left off (test
# stays on) exactly as the BLEND case does -- a halo must not occlude the core
# it surrounds, and additive is order-independent for colour.
# THE MARK MUST BE TEXT THAT THE REPLACEMENT ACTUALLY CONTAINS. The first cut
# checked for `extras.get("blendMode")` while inserting
# `(material.extras or {}).get("blendMode")` -- which does not contain it -- so
# the guard never matched and the block stacked once per build (12 copies before
# it was noticed). Idempotence here is not a nicety: build_banner.sh runs this
# on every single build.
BLEND_MARK = 'extras.blendMode: "ADD"'
BLEND_ANCHOR = """                # multiply base texture with primary color
                mtob.fragment_shader.texture_combiners[0].src_rgb = 0x030"""
BLEND_REPLACE = """                # glTF `extras.blendMode: "ADD"` -> ADDITIVE (One/One), depth
                # test on / write off (added by tools/patch_pycgfx.py).
                if (material.extras or {}).get("blendMode") == "ADD":
                    bo = mtob.fragment_operations.blend_operation
                    bo.src_color = BlendFunction.One
                    bo.dst_color = BlendFunction.One
                    bo.src_alpha = BlendFunction.One
                    bo.dst_alpha = BlendFunction.One
                    mtob.fragment_operations.depth_operation.flags = (
                        DepthFlag.TestEnabled
                    )

                # multiply base texture with primary color
                mtob.fragment_shader.texture_combiners[0].src_rgb = 0x030"""

ANCHOR = """                if pmr.baseColorFactor:
                    mtob.material_color.diffuse = ColorFloat(*pmr.baseColorFactor)
"""

ADD = """
                # glTF `emissiveFactor` -> the PICA material's EMISSION term
                # (added by tools/patch_pycgfx.py; see that file for why).
                if material.emissiveFactor:
                    mtob.material_color.emission = ColorFloat(
                        *material.emissiveFactor, 1
                    )
"""


def main(path: str) -> int:
    src = open(path).read()
    changed = []

    if MARK not in src:
        if ANCHOR not in src:
            print("patch_pycgfx: ANCHOR not found in %s -- upstream changed, "
                  "re-derive the patch" % path, file=sys.stderr)
            return 1
        src = src.replace(ANCHOR, ANCHOR + ADD, 1)
        changed.append("emissiveFactor")

    if SORT_MARK not in src:
        if SORT_ANCHOR not in src:
            print("patch_pycgfx: bone-sort ANCHOR not found -- upstream "
                  "changed, re-derive the patch", file=sys.stderr)
            return 1
        src = src.replace(SORT_ANCHOR, SORT_REPLACE, 1)
        changed.append("bone-sort disabled")

    if BILLBOARD_MARK not in src:
        if BILLBOARD_ANCHOR not in src:
            print("patch_pycgfx: billboard ANCHOR not found -- upstream "
                  "changed, re-derive the patch", file=sys.stderr)
            return 1
        src = src.replace(BILLBOARD_ANCHOR, BILLBOARD_REPLACE, 1)
        changed.append("billboardMode")

    if BLEND_MARK not in src:
        if BLEND_ANCHOR not in src:
            print("patch_pycgfx: blendMode ANCHOR not found -- upstream "
                  "changed, re-derive the patch", file=sys.stderr)
            return 1
        src = src.replace(BLEND_ANCHOR, BLEND_REPLACE, 1)
        changed.append("blendMode ADD")

    if SPEC_MARK not in src:
        if SPEC_ANCHOR not in src:
            print("patch_pycgfx: specular ANCHOR not found -- upstream "
                  "changed, re-derive the patch", file=sys.stderr)
            return 1
        src = src.replace(SPEC_ANCHOR, SPEC_REPLACE, 1)
        changed.append("extras.specular")

    if not changed:
        print("patch_pycgfx: already applied")
        return 0
    open(path, "w").write(src)
    print("patch_pycgfx: applied -- %s" % ", ".join(changed))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
