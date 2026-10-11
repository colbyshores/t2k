# T2K — top-level build driver. Each target delegates to its own build system:
#
#   make 3ds     -> t2k_3ds/Makefile   (devkitARM, libctru + Citro3D) -> t2k_3ds/t2k.3dsx
#   make cia     -> the same, packaged installable WITH the soundtrack and a
#                   first-boot config in its romfs      -> t2k_3ds/t2k.cia
#   make production -> the DETERMINISTIC path to a shippable t2k.cia: refuses
#                   a dirty tree, always cleans first, verifies the banner is
#                   16-bit and the exheader maps DSP RAM (both from source AND
#                   from the built bytes), and logs commit+sha256 to
#                   docs/validation/cia-production-builds.log. Use this, not a
#                   bare `make cia`, for anything leaving this machine. See
#                   DOCTRINE.md "The installable build" / "build production".
#   make pc      -> t2k_pc/CMakeLists  (CMake, SDL2 + desktop renderer) -> t2k_pc/build/t2k
#   make music   -> soundtracks/build.py --regen: rebuild + verify the soundtrack
#                   pool from the pinned map, refresh the display titles, and run
#                   the music gates. Touches no compiler; deliberately NOT part
#                   of `all` (see the target below for what it needs).
#   make all     -> both
#   make clean   -> both build trees
#
# The game itself lives in t2k_core/ (platform-agnostic: logic, data, shared
# geometry builders, seam-only UI) and is compiled into BOTH targets; each
# t2k_<target>/ tree holds only that platform's layer. See DOCTRINE.md
# "Repository layout".
#
# Environment: `make 3ds` needs DEVKITPRO / DEVKITARM (defaults below match
# the stock /opt/devkitpro install). `make pc` needs cmake >= 3.16.

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
export DEVKITPRO DEVKITARM
export PATH := $(PATH):$(DEVKITPRO)/tools/bin:$(DEVKITARM)/bin

JOBS      ?= $(shell nproc 2>/dev/null || echo 4)
PC_BUILD  := t2k_pc/build
PC_CMAKE_ARGS ?=

.PHONY: all 3ds cia production pc music check clean clean-3ds clean-pc

all: pc 3ds

3ds:
	$(MAKE) -C t2k_3ds -j$(JOBS)

# Bundles the gitignored data/music/ pool -- ~300 MB of copyrighted audio -- so
# the artifact is personal. See t2k_3ds/cia/README.md.
cia:
	$(MAKE) -C t2k_3ds -j$(JOBS) cia

# The deterministic, traceable production path -- see t2k_3ds/tools/build_production.sh.
production:
	@t2k_3ds/tools/build_production.sh

pc:
	cmake -S t2k_pc -B $(PC_BUILD) $(PC_CMAKE_ARGS)
	cmake --build $(PC_BUILD) -j$(JOBS)

# The soundtrack pipeline in one command: vendored encoder -> display titles ->
# pool (FLAC -> DSP-ADPCM, every file hash-checked against soundtracks/track_map.json)
# -> album maps -> the music gates. Needs the gitignored source FLACs under
# soundtracks/ (~230 MB, see soundtracks/download.py) and ffmpeg on PATH; without
# them it stops and prints what is still possible from committed files alone
# (`make music` is then `python3 soundtracks/build.py --regen --titles-only`).
# NOT a prerequisite of `all`: it writes 230 MB of copyrighted audio, and a build
# of the game must not depend on having the right to ship it.
music:
	@python3 soundtracks/build.py --regen

# Gate suite (docs/PRISTINE-SPEC.md 1): builds both targets, then runs every
# verification gate and fails on the first failure. Deliberately NOT wired into
# `pc`/`3ds` -- nothing gains the ability to fail that did not have it before.
check:
	@bash tools/check.sh

clean: clean-3ds clean-pc

clean-3ds:
	$(MAKE) -C t2k_3ds clean

clean-pc:
	rm -rf $(PC_BUILD)
