#!/usr/bin/env bash
# build_banner.sh -- build the HOME-menu banner MODEL (banner.cgfx).
#
# The banner is not a picture. It is a CGFX scene the HOME menu renders and
# whose animations it loops, which is why retail banners move and homebrew ones
# (built from bannertool's `-i banner.png` path, a flat quad with the PNG
# pasted on) do not. This script produces the real thing:
#
#   tools/gen_banner_model.py   the game's own square web + T2K wordmark,
#                               read from levels.json / logo_data.h /
#                               web_palette.h, as an animated glTF
#   pycgfx                      glTF -> CGFX (skyfloogle/pycgfx)
#
# and the Makefile hands the result to bannertool with `-ci`.
#
# Like get_cia_tools.sh, everything it needs is fetched into tools/.ciatools/
# (gitignored) on first use. Set PYCGFX= to point at your own checkout.
#
#---------------------------------------------------------------------------------
# PINNED REVISIONS AND PACKAGE VERSIONS
#
# pycgfx used to be cloned `--depth 1` off the default branch and its three PyPI
# deps were installed unpinned, so the banner -- a build artefact that ships in
# the CIA -- depended on whatever GitHub and PyPI were serving that day. Both are
# pinned now (2026-10-11), to the state measured on the machine that produced the
# banner currently on the console, so this freezes behaviour rather than changing
# it.
#
#   pycgfx  skyfloogle/pycgfx  1f78850086f3a77c41e07162e842f97a5bf3c18a
#           main HEAD as read with `git ls-remote` on 2026-10-11 ("mention the
#           much more easily googleable bcres", 2025-06-02). The repo publishes
#           NO tags at all, so the commit SHA is the only pin available.
#
#   venv    gltflib==1.0.13  pillow==12.3.0  numpy==2.5.3
#           The versions installed in tools/.ciatools/pycgfx-venv when the
#           shipped banner.cgfx was produced. pillow is the one that matters
#           most: gen_banner_level.py rasterises the level's texture through it
#           and pycgfx encodes it, so a resampler or PNG-encoder change moves
#           bytes in banner.cgfx -> banner.bnr -> the CIA. That is a banner BYTE
#           change, not a cosmetic one.
#           gltflib's own closure (dataclasses-json 0.6.7, marshmallow 3.26.2,
#           typing_extensions 4.16.0, typing_inspect 0.9.0, mypy_extensions
#           1.1.0) is recorded here unpinned; if a banner byte ever moves with
#           all three pins held, that closure is the next thing to freeze.
#
# Bump deliberately: PYCGFX_REV=<sha> tools/build_banner.sh out.cgfx
# A checkout or venv that does not match its pin is a hard error -- see
# get_cia_tools.sh, which uses the same rule for makerom and bannertool.
#---------------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="$HERE/.ciatools"
# Absolute: the converter is run from its own checkout directory, so every
# path handed to it has to survive the cd.
OUT="${1:?usage: build_banner.sh <output .cgfx path>}"
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
WORK="$(dirname "$OUT")/banner-model"

say() { echo "$@" >&2; }

PYCGFX="${PYCGFX:-$CACHE/pycgfx}"
PYCGFX_REV="${PYCGFX_REV:-1f78850086f3a77c41e07162e842f97a5bf3c18a}"

# Same rule as get_cia_tools.sh: fetch the COMMIT, not a branch tip, and refuse
# to continue if the checkout does not report the pinned SHA. A stale cache
# directory is checked too -- "the directory exists" is not evidence it is the
# revision the banner was built with.
if [ -d "$PYCGFX/.git" ]; then
    say "==> reusing $PYCGFX"
else
    say "==> fetching pycgfx (skyfloogle/pycgfx) @ $PYCGFX_REV into $CACHE"
    mkdir -p "$CACHE"
    git init -q "$PYCGFX" >&2
    git -C "$PYCGFX" remote add origin https://github.com/skyfloogle/pycgfx.git >&2
    git -C "$PYCGFX" fetch -q --depth 1 origin "$PYCGFX_REV" >&2
    git -C "$PYCGFX" checkout -q FETCH_HEAD >&2
fi
PYCGFX_GOT="$(git -C "$PYCGFX" rev-parse HEAD 2>/dev/null || echo none)"
if [ "$PYCGFX_GOT" != "$PYCGFX_REV" ]; then
    say "PIN VIOLATION: $PYCGFX is at $PYCGFX_GOT but this script pins $PYCGFX_REV"
    say "  Bump it deliberately (PYCGFX_REV=<sha>) or remove the directory."
    exit 1
fi
say "     $PYCGFX @ $PYCGFX_GOT (pinned)"

# The one local change the converter needs: honour glTF emissiveFactor, or
# every surface angled away from the scene's single light renders black and the
# neon look is gone. See tools/patch_pycgfx.py. Idempotent.
python3 "$HERE/patch_pycgfx.py" "$PYCGFX/main.py" >&2

VENV="$CACHE/pycgfx-venv"
PYCGFX_PKGS=("gltflib==1.0.13" "pillow==12.3.0" "numpy==2.5.3")
if [ ! -x "$VENV/bin/python" ]; then
    say "==> creating $VENV (${PYCGFX_PKGS[*]})"
    python3 -m venv "$VENV" >&2
    "$VENV/bin/pip" install -q --disable-pip-version-check "${PYCGFX_PKGS[@]}" >&2
fi
# An existing venv predates the pins, and its existence is not evidence it
# matches them. Check every pin and refuse rather than emit a banner whose bytes
# came from a different pillow than the manifest claims.
for spec in "${PYCGFX_PKGS[@]}"; do
    pkg="${spec%%==*}"; want="${spec##*==}"
    have="$("$VENV/bin/python" -c "import importlib.metadata as m; print(m.version('${pkg}'))" 2>/dev/null || echo none)"
    if [ "$have" != "$want" ]; then
        say "PIN VIOLATION: $VENV has $pkg $have, this script pins $want"
        say "  rm -rf $VENV and re-run to rebuild it at the pinned versions."
        exit 1
    fi
done
say "     venv ok: ${PYCGFX_PKGS[*]}"

# WHICH BANNER. `gen_banner_level.py` is the one that ships: one LEVEL'S tube,
# wearing that level's own procedural skin, turning the way the level-select
# screen turns it (user, 2026-09-08). `gen_banner_model.py` is the earlier
# icon-composition banner -- the square web head-on with the T2K wordmark
# across the bottom, static -- kept because it is a different design rather
# than a worse version of this one. BANNER_GEN= selects it.
BANNER_GEN="${BANNER_GEN:-gen_banner_level.py}"
BANNER_LEVEL="${BANNER_LEVEL:-15}"
# Where the level's colour band is spent across the model: "lane" (the default)
# gives one hue per lane around the ring with the outline as the subject;
# "depth" spends it down the barrel instead. See gen_banner_level.py's
# BAND_AXIS_MODE.
BANNER_SHADING="${BANNER_SHADING:-lane}"
# Extra generator flags, for BISECTING a banner the console rejects. The HOME
# menu gives no error -- it freezes -- so the only way to find what it will not
# accept is to remove one thing at a time and reinstall. `--no-spin` (drop the
# animation entirely) is the first cut; `--frames N` shortens the loop.
# THE SHIPPED BANNER'S SETTINGS LIVE HERE, NOT IN SOMEONE'S SHELL HISTORY.
# These three are what the approved banner on the console is built from, and
# they are DEFAULTS rather than an env var a release build has to remember,
# because the generator's own defaults (cols 5, segs 8, 128x256) plus the
# additive glow come to ~614 KB and the 512 KB cap would refuse the build --
# i.e. forgetting them does not degrade the banner, it fails the release.
#
# Why these values: the glow multiplied the geometry, so the budget had to come
# from somewhere, and it came from the two axes that cost the most and show the
# least at 256 px (lane columns and depth bands) plus the texture, rather than
# from the strokes or the glow, which are the whole look. 400 KB with ~110 KB
# of headroom. Override for experiments; do not "tidy" them away.
BANNER_EXTRA="${BANNER_EXTRA:---tex 64x64 --cols 3 --segs 4}"

mkdir -p "$WORK"
say "MDL  $WORK/banner.gltf  ($BANNER_GEN)"
if [ "$BANNER_GEN" = "gen_banner_level.py" ]; then
    python3 "$HERE/$BANNER_GEN" --out "$WORK" --level "$BANNER_LEVEL" \
            --shading "$BANNER_SHADING" $BANNER_EXTRA >&2
else
    python3 "$HERE/$BANNER_GEN" --out "$WORK" >&2
fi

say "CGFX $OUT"
( cd "$PYCGFX" && "$VENV/bin/python" main.py "$WORK/banner.gltf" >&2 )
mv "$WORK/banner.cgfx" "$OUT"

# The 3DS will not load a banner CGFX over 512 KB. pycgfx warns; we refuse,
# because a banner that silently fails to load is invisible until the title is
# already installed on a console.
SZ=$(stat -c%s "$OUT")
if [ "$SZ" -gt 524288 ]; then
    say "build_banner: $OUT is $SZ bytes -- the 3DS caps banner CGFX at 512 KB"
    exit 1
fi
say "     $OUT ok ($SZ bytes)"
