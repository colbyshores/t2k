#
# t2k_3ds/Makefile — Tube Shooter Nintendo 3DS port (devkitPro / devkitARM).
#
# Usage (from the repo root, via the top-level Makefile):
#   make 3ds                          # release build (Citro3D / PICA200)
#   make -C t2k_3ds DEBUG=1           # debug build (__3DS_DEBUG__ defined)
#   make -C t2k_3ds clean
#
# Sources come from TWO trees: ../t2k_core/src (the platform-agnostic game --
# logic, data, shared geometry builders, seam-only UI) and ./src (this
# target's platform layer: main_3ds, the Citro3D backend, ndsp audio sinks).
# The desktop's SDL/GL files live in ../t2k_pc and are never globbed here, so
# the old EXCLUDE_SRC list is empty by construction.
#
# Modeled on Forsaken's Makefile.3ds (the multi-backend "rosetta stone" for
# 3DS porting) and Perfect Dark's Makefile.3ds. See DOCTRINE.md.
#
# Produces a complete, playable t2k.3dsx. `make clean` is mandatory when
# flipping any -D flag — this Makefile does not track flag changes.
#

DEBUG ?= 0

#---------------------------------------------------------------------------------
# Environment
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM. export DEVKITARM=<path to>/devkitARM")
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO. export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Build configuration
#---------------------------------------------------------------------------------
SRCDIR   := $(CURDIR)/src
CORE     := $(abspath $(CURDIR)/../t2k_core)
CORESRC  := $(CORE)/src
TARGET   := t2k
BUILD    := build
PORTLIBS := $(DEVKITPRO)/portlibs/3ds

#---------------------------------------------------------------------------------
# Sources — every .cpp under ../t2k_core/src plus every .cpp under ./src.
# Nothing desktop-only can be reached from here (it lives in ../t2k_pc), so
# there is nothing to exclude; the variable is kept so a future 3DS-side
# opt-out has an obvious home.
#---------------------------------------------------------------------------------
EXCLUDE_SRC :=

# THE WILDCARD IS PER-DEPTH, and that is a real trap: it is a list of explicit
# globs, not a recursive walk, so a source in a NEW nesting level is silently
# dropped -- the 3DS build stays green and simply links a different game.
# src/game/enemies/ (the per-family enemy TUs, see enemies/enemy_family.h) is
# the third level, hence the third glob. Add a fourth here before adding one to
# the tree.
#
# NB `notdir` means BASENAMES MUST BE UNIQUE across the whole subtree -- two
# files called reflector.cpp in different directories would collapse into one
# object and one of them would never be compiled.
SRC_GLOBS := $(wildcard $(CORESRC)/*.cpp) \
             $(wildcard $(CORESRC)/*/*.cpp) \
             $(wildcard $(CORESRC)/*/*/*.cpp) \
             $(wildcard $(SRCDIR)/*.cpp) \
             $(wildcard $(SRCDIR)/*/*.cpp) \
             $(wildcard $(SRCDIR)/*/*/*.cpp)

ALL_SRC := $(notdir $(SRC_GLOBS))
CPPFILES := $(filter-out $(EXCLUDE_SRC),$(ALL_SRC))
OFILES   := $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o))

# Let make find the sources across the src/ subtree.
VPATH := $(sort $(dir $(SRC_GLOBS)))

#---------------------------------------------------------------------------------
# Flags — ARMv6K (ARM11), hard float, VFP. Peak-performance / no-C++-overhead
# discipline: exceptions and RTTI OFF (see DOCTRINE.md performance doctrine).
#---------------------------------------------------------------------------------
ARCH := -march=armv6k -mtpcs-leaf-frame -mfloat-abi=hard -mtune=mpcore -mfpu=vfp


# Which of these src/ ACTUALLY consumes, checked 2026-09-09 -- stated because
# DOCTRINE.md's own build-flag trap cuts both ways: a macro renamed on one side
# builds green and silently compiles a feature out, and a macro nothing reads
# looks like a live switch to the next reader.
#   __3DS__ (2 files), ARM (3), PLATFORM_3DS (1), RENDERER_C3D (2) -- LIVE.
#   PLATFORM_ARM -- consumed by NOTHING today, and KEPT anyway: DOCTRINE.md
#     requires the OS/API pair (__3DS__ / PLATFORM_3DS) and the CPU pair
#     (ARM / PLATFORM_ARM) stay independent, so this is reserved headroom for
#     CPU-gated code, not an oversight. Do not "tidy" it away.
DEFINES := -D__3DS__ -DARM -DPLATFORM_3DS -DPLATFORM_ARM -DRENDERER_C3D
ifeq ($(DEBUG),1)
# Also consumed by nothing today. Kept because `make DEBUG=1` is the documented
# way in, and a debug build that defines nothing is cheaper than one that has to
# be re-invented; but DO NOT assume DEBUG=1 changes any source behaviour -- as
# of 2026-09-09 it only adds this define and whatever the flags below do.
DEFINES += -D__3DS_DEBUG__
endif

COMMONFLAGS := $(ARCH) -O2 -ffunction-sections -fdata-sections -fno-math-errno -fno-strict-aliasing $(DEFINES)
INCLUDE := -I$(CORESRC) \
	-I$(SRCDIR) \
	-I$(BUILD) \
	-I$(CORE)/third_party/glm \
	-I$(CORE)/third_party \
	-I$(DEVKITPRO)/libctru/include \
	-I$(PORTLIBS)/include

CXXFLAGS := $(COMMONFLAGS) $(INCLUDE) -std=gnu++17 -fno-exceptions -fno-rtti \
	-Wall -Wextra -Wno-unused-parameter

LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,-Map,$(TARGET).map \
	-L$(DEVKITPRO)/libctru/lib -L$(PORTLIBS)/lib
LIBS := -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# PICA200 vertex shader (procedural texturing runs on TEV; only vertex work is
# programmable). Compiled by picasso, embedded into an object + header via bin2s
# so renderer_c3d.cpp can #include "ts_c3d_shbin.h".
#---------------------------------------------------------------------------------
PICASSO   := $(DEVKITPRO)/tools/bin/picasso
BIN2S     := $(DEVKITPRO)/tools/bin/bin2s
SHADER_PICA  := $(CURDIR)/shaders/ts_c3d.v.pica
SHADER_SHBIN := $(BUILD)/ts_c3d.shbin
SHADER_HDR   := $(BUILD)/ts_c3d_shbin.h
SHADER_OBJ   := $(BUILD)/ts_c3d_shbin.o
GRID_SHADER_PICA  := $(CURDIR)/shaders/ts_c3d_grid.v.pica
GRID_SHADER_SHBIN := $(BUILD)/ts_c3d_grid.shbin
GRID_SHADER_HDR   := $(BUILD)/ts_c3d_grid_shbin.h
GRID_SHADER_OBJ   := $(BUILD)/ts_c3d_grid_shbin.o
PART_SHADER_PICA  := $(CURDIR)/shaders/ts_c3d_part.v.pica
PART_SHADER_SHBIN := $(BUILD)/ts_c3d_part.shbin
PART_SHADER_HDR   := $(BUILD)/ts_c3d_part_shbin.h
PART_SHADER_OBJ   := $(BUILD)/ts_c3d_part_shbin.o
RIVER_SHADER_PICA  := $(CURDIR)/shaders/ts_c3d_river.v.pica
RIVER_SHADER_SHBIN := $(BUILD)/ts_c3d_river.shbin
RIVER_SHADER_HDR   := $(BUILD)/ts_c3d_river_shbin.h
RIVER_SHADER_OBJ   := $(BUILD)/ts_c3d_river_shbin.o

# PoC (poc/gpu-tick-lerp): GPU-side line-segment expansion. See the shader's
# own header -- it moves the per-eye project+expand loop off the CPU, which
# measured 85.7% of the `line` stage on the OG profile.
SEG_SHADER_PICA    := $(CURDIR)/shaders/ts_c3d_seg.v.pica
SEG_SHADER_SHBIN   := $(BUILD)/ts_c3d_seg.shbin
SEG_SHADER_HDR     := $(BUILD)/ts_c3d_seg_shbin.h
SEG_SHADER_OBJ     := $(BUILD)/ts_c3d_seg_shbin.o

#---------------------------------------------------------------------------------
# Level data — ../t2k_core/data/levels.json is THE level data. Embed it verbatim as the
# fallback + the SD-card first-boot seed; tools/bin2h.py validates it first,
# so a broken levels.json fails the build instead of a boot.
#---------------------------------------------------------------------------------
LEVELS_JSON := $(CORE)/data/levels.json
LEVELS_HDR  := $(BUILD)/levels_json.h

#---------------------------------------------------------------------------------
# Album map — ../t2k_core/data/albums_manifest.json embedded as the fallback for
# a card that has music but no albums.json beside it (that file is gitignored:
# it lives with the audio). The card's own copy still wins at load.
#---------------------------------------------------------------------------------
ALBUMS_JSON := $(CORE)/data/albums_manifest.json
ALBUMS_HDR  := $(BUILD)/albums_json.h

#---------------------------------------------------------------------------------
# Targets
#---------------------------------------------------------------------------------
.PHONY: all clean
all: $(TARGET).3dsx

$(BUILD):
	@mkdir -p $(BUILD)

# Compile the PICA vertex shader, then embed it (bin2s -> .s + header -> .o).
# Symbols produced: ts_c3d_shbin[], ts_c3d_shbin_size.
$(SHADER_SHBIN): $(SHADER_PICA) | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $@ $<

# &: (grouped target, GNU Make 4.3+) is load-bearing here, NOT stylistic: with
# plain ':', two targets sharing one recipe are not atomic, so `make -j` can
# schedule this recipe TWICE concurrently (once for whichever target a
# parallel job needs first) -- both instances race to write the same
# ts_c3d_shbin.s in $(BUILD), and the loser's truncated write can pair a
# corrupt .s with a header claiming the old (correct) _size. Reproduced on
# GNU Make 4.3: two concurrent recipe runs, PIDs microseconds apart. `make
# clean` if you ever see a DVLB parse failure that a serial rebuild fixes.
$(SHADER_HDR) $(SHADER_OBJ) &: $(SHADER_SHBIN)
	@echo "SHBN $(SHADER_SHBIN)"
	@cd $(BUILD) && $(BIN2S) -H ts_c3d_shbin.h ts_c3d.shbin > ts_c3d_shbin.s
	@$(CC) $(ARCH) -x assembler-with-cpp -c $(BUILD)/ts_c3d_shbin.s -o $(SHADER_OBJ)

# Grid wave vertex shader (3DS perf lever) — its own shbin/DVLB.
$(GRID_SHADER_SHBIN): $(GRID_SHADER_PICA) | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $@ $<

# &: for the same reason as the shader rule above.
$(GRID_SHADER_HDR) $(GRID_SHADER_OBJ) &: $(GRID_SHADER_SHBIN)
	@echo "SHBN $(GRID_SHADER_SHBIN)"
	@cd $(BUILD) && $(BIN2S) -H ts_c3d_grid_shbin.h ts_c3d_grid.shbin > ts_c3d_grid_shbin.s
	@$(CC) $(ARCH) -x assembler-with-cpp -c $(BUILD)/ts_c3d_grid_shbin.s -o $(GRID_SHADER_OBJ)

# Particle/shatter dot vertex shader (packed 20-byte vertex) — its own shbin/DVLB.
$(PART_SHADER_SHBIN): $(PART_SHADER_PICA) | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $@ $<

# &: for the same reason as the shader rule above.
$(PART_SHADER_HDR) $(PART_SHADER_OBJ) &: $(PART_SHADER_SHBIN)
	@echo "SHBN $(PART_SHADER_SHBIN)"
	@cd $(BUILD) && $(BIN2S) -H ts_c3d_part_shbin.h ts_c3d_part.shbin > ts_c3d_part_shbin.s
	@$(CC) $(ARCH) -x assembler-with-cpp -c $(BUILD)/ts_c3d_part_shbin.s -o $(PART_SHADER_OBJ)

# GATES river vertex shader (two texcoord sets for the dual-texture TEV chain)
# — its own shbin/DVLB.
$(RIVER_SHADER_SHBIN): $(RIVER_SHADER_PICA) | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $@ $<

# &: for the same reason as the shader rule above.
$(RIVER_SHADER_HDR) $(RIVER_SHADER_OBJ) &: $(RIVER_SHADER_SHBIN)
	@echo "SHBN $(RIVER_SHADER_SHBIN)"
	@cd $(BUILD) && $(BIN2S) -H ts_c3d_river_shbin.h ts_c3d_river.shbin > ts_c3d_river_shbin.s
	@$(CC) $(ARCH) -x assembler-with-cpp -c $(BUILD)/ts_c3d_river_shbin.s -o $(RIVER_SHADER_OBJ)

$(SEG_SHADER_SHBIN): $(SEG_SHADER_PICA) | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $@ $<

# &: for the same reason as the shader rules above.
$(SEG_SHADER_HDR) $(SEG_SHADER_OBJ) &: $(SEG_SHADER_SHBIN)
	@echo "SHBN $(SEG_SHADER_SHBIN)"
	@cd $(BUILD) && $(BIN2S) -H ts_c3d_seg_shbin.h ts_c3d_seg.shbin > ts_c3d_seg_shbin.s
	@$(CC) $(ARCH) -x assembler-with-cpp -c $(BUILD)/ts_c3d_seg_shbin.s -o $(SEG_SHADER_OBJ)

$(LEVELS_HDR): $(LEVELS_JSON) $(CORE)/tools/bin2h.py | $(BUILD)
	@echo "EMBD t2k_core/data/levels.json"
	@python3 $(CORE)/tools/bin2h.py $(LEVELS_JSON) $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "CXX  $<"
	@$(CXX) -MMD -MP -MF $(BUILD)/$*.d $(CXXFLAGS) -c $< -o $@

# renderer_c3d.cpp includes the generated shader header.
$(BUILD)/renderer_c3d.o: $(SHADER_HDR) $(GRID_SHADER_HDR) $(PART_SHADER_HDR) $(RIVER_SHADER_HDR) $(SEG_SHADER_HDR)

$(ALBUMS_HDR): $(ALBUMS_JSON) $(CORE)/tools/gen_albums.py | $(BUILD)
	@echo "EMBD t2k_core/data/albums_manifest.json"
	@python3 $(CORE)/tools/gen_albums.py --emit-header $@

# webs_runtime.cpp includes the embedded level data; music_core.cpp includes
# the embedded album map.
$(BUILD)/webs_runtime.o: $(LEVELS_HDR)
$(BUILD)/music_core.o: $(ALBUMS_HDR)

# menu.cpp shows the build tag (git rev + date) on the title screen and
# renderer_c3d.cpp stamps it into tex.log / perf.log, so a console can always
# say which build it is running -- TWO consumers, and both need the generated
# header before they compile. Regenerated EVERY build; the header is rewritten
# only when its content changes, so those objects recompile only when the
# revision moves.
BUILD_REV_HDR := $(BUILD)/build_rev.h
.PHONY: FORCE
$(BUILD_REV_HDR): FORCE | $(BUILD)
	@python3 $(CORE)/tools/gen_build_rev.py $@
$(BUILD)/menu.o: $(BUILD_REV_HDR)
$(BUILD)/renderer_c3d.o: $(BUILD_REV_HDR)

$(TARGET).elf: $(OFILES) $(SHADER_OBJ) $(GRID_SHADER_OBJ) $(PART_SHADER_OBJ) $(RIVER_SHADER_OBJ) $(SEG_SHADER_OBJ)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OFILES) $(SHADER_OBJ) $(GRID_SHADER_OBJ) $(PART_SHADER_OBJ) $(RIVER_SHADER_OBJ) $(SEG_SHADER_OBJ) $(LIBS) -o $@

$(TARGET).3dsx: $(TARGET).elf
	@echo "3DSX $@"
	@3dsxtool $< $@

#---------------------------------------------------------------------------------
# CIA — the installable title, with the soundtrack and a first-boot config
# BUNDLED IN ITS ROMFS. `make cia` (or `make -C t2k_3ds cia`).
#
# The .3dsx above stays the development build and is unchanged: it has no
# romfs, and every use of one in the game is conditional on the mount, so the
# two builds are the same game reading content from different places.
#
# NEITHER makerom NOR bannertool SHIPS WITH devkitPro, and bannertool's
# original repo is gone from GitHub -- tools/get_cia_tools.sh builds both into
# tools/.ciatools/ on first use and prints where they landed. Override with
# `make cia MAKEROM=... BANNERTOOL=...`.
#
# THE ROMFS IS ~300 MB OF COPYRIGHTED AUDIO read from the gitignored
# data/music/ pool. Nothing it carries is in this repository, and the .cia it
# produces is a personal artifact -- do not publish it.
#---------------------------------------------------------------------------------
CIA_DIR       := $(CURDIR)/cia
CIA_ROMFS     := $(BUILD)/romfs
CIA_BANNER    := $(BUILD)/banner.bnr
CIA_BANNERCGFX:= $(BUILD)/banner.cgfx
CIA_SMDH      := $(BUILD)/$(TARGET).smdh

# THE TITLE'S IDENTITY ON THE HOME MENU IS THREE AUTHORED STRINGS.
# smdhtool takes a short description, a long description and a publisher, and
# the HOME menu shows the LONG one under the icon when you hover it.
#
# THIS USED TO BE ONE NAME USED THREE TIMES, AND THAT WAS DELIBERATE -- do not
# read the change below as the old rule being forgotten. Three separate strings
# were three chances to show something other than the game's name, and all three
# had gone wrong at once: the menu read "Neon tube shooter" and the publisher
# carried a person's name that nobody had chosen to ship. Collapsing them onto
# APP_TITLE made that class of mistake impossible.
#
# The user has now AUTHORED all three on purpose (2026-09-08), which is the
# thing the collapse was standing in for until someone did. The publisher is a
# real name by explicit instruction -- it is no longer a leak, it is a credit.
#
# THE PUBLISHER IS "TSEngine Devs", NOT "TSEngine" (user, 2026-09-27; codified
# in AGENTS.md §9). The engine's internal name is not what ships on the home
# menu: every new CIA must credit "TSEngine Devs" in the publisher field. The
# guard reads this value and re-checks the built SMDH against it, so a CIA that
# says "TSEngine" (or anything else) fails the production build rather than
# reaching the console.
#
# The guard did not go away, it got sharper: it now checks each field against
# its OWN expected string rather than against one shared name, so a stale or
# swapped value still fails. Checked, not just written -- build_production.sh
# refuses a build whose Makefile disagrees, then re-reads the SMDH out of the
# BUILT CIA and refuses if the bytes say anything else
# (tools/check_cia_smdh.py). Same discipline as the DSP mapping: the spec that
# asked for it is not evidence that it happened.
APP_TITLE        := T2K
APP_DESCRIPTION  := Psychedelic Tube Shooter
APP_AUTHOR       := TSEngine Devs
# UniqueId is the title's identity to the console: change it and an install
# becomes a SECOND, separate title rather than an update of the first (the old
# one keeps its save data and stays on the menu). 0xF7A2C is in the homebrew
# range and is arbitrary but FIXED -- pick once, never move.
APP_UNIQUE_ID    := 0xF7A2C
APP_PRODUCT_CODE := CTR-P-the reference build

# ---- WHICH BANNER GOES IN -- AND WHY png IS THE DEFAULT ----------------------
# BANNER_MODE=png    a still, through bannertool's own template CGFX. What
#                    every homebrew title ships and the only form of this that
#                    has ever run on a real console.
# BANNER_MODE=model  the 3D model (tools/build_banner.sh). UNPROVEN ON
#                    HARDWARE, and worse than unproven: on 2026-09-08 the first
#                    installed CIA carrying it SOFTLOCKED the HOME menu when the
#                    icon was hovered -- the banner audio played and then the
#                    console hung hard.
#
# THE MODEL IS NOW THE DEFAULT -- IT IS PROVEN ON HARDWARE (2026-09-08).
# The paragraph above is kept because its reasoning was right and the softlock
# was real; what changed is the evidence. Four causes were found and fixed (a
# non-power-of-two texture, the texture size, pycgfx's bone re-sort, and a
# preview that under-reported vertical risk), and the model has since been
# installed and hovered repeatedly on the user's console: it spins, the wordmark
# holds still, and the glow reads. A banner is still the one thing here that
# runs before the game does, on the system menu's own thread -- so if it ever
# regresses, `BANNER_MODE=png` is one variable away and cia/banner.png is kept
# for exactly that.
BANNER_MODE ?= model
ifeq ($(BANNER_MODE),model)
CIA_BANNER_ARG := -ci $(CIA_BANNERCGFX)
CIA_BANNER_DEP := $(CIA_BANNERCGFX)
else
CIA_BANNER_ARG := -i $(CIA_DIR)/banner.png
CIA_BANNER_DEP := $(CIA_DIR)/banner.png
endif

.PHONY: cia
cia: $(TARGET).cia

# THE SMDH DEPENDS ON THE NAME, NOT JUST ON THE ART. Its inputs are two PNGs
# and three STRINGS, and make cannot see a string -- so `make cia APP_TITLE=...`
# happily reused an SMDH built under the old name and shipped a title calling
# itself the wrong thing, with every check downstream passing because the bytes
# were internally consistent. Same shape as the stale-.o-after-a-rename trap.
# The stamp makes the strings a FILE, which make can see.
# FORCE, so the stamp's recipe is re-evaluated every build; `cmp` then leaves
# the file's mtime ALONE unless the strings actually changed, so the SMDH
# rebuilds when the name moves and not otherwise. Without FORCE the stamp is an
# ordinary file with no prerequisites -- make sees it exists, never re-runs it,
# and the staleness it was added to catch survives it.
.PHONY: FORCE
FORCE:

$(BUILD)/smdh.stamp: FORCE | $(BUILD)
	@printf '%s\n' "$(APP_TITLE)|$(APP_DESCRIPTION)|$(APP_AUTHOR)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(CIA_SMDH): $(CIA_DIR)/icon.png $(CIA_DIR)/icon_small.png $(BUILD)/smdh.stamp | $(BUILD)
	@echo "SMDH $@"
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" \
	          $(CIA_DIR)/icon.png $@ $(CIA_DIR)/icon_small.png

# .PHONY on the romfs: the pool it stages from is outside the repo and can
# change under us (a track added, an album re-cut), and make cannot depend on
# a directory's CONTENTS. Restaging is seconds -- it hard-links.
.PHONY: $(CIA_ROMFS)
$(CIA_ROMFS): $(CIA_DIR)/default_config.json | $(BUILD)
	@echo "RFS  $@"
	@$(CURDIR)/tools/stage_romfs.sh $@

# THE HOME-MENU BANNER IS A 3D MODEL, NOT A PICTURE -- level 15's own tube,
# textured with that level's own procedural skin, turning the way the
# level-select screen turns it (tools/gen_banner_level.py, converted by
# pycgfx). BANNER_GEN= selects the earlier icon-composition banner instead;
# see cia/README.md. bannertool's `-i <png>` path would paste a still onto its
# flat template quad, which is why every homebrew banner is static.
# cia/banner.png is that old flat banner, kept as the documented fallback.
.PHONY: $(CIA_BANNERCGFX)
$(CIA_BANNERCGFX): | $(BUILD)
	@$(CURDIR)/tools/build_banner.sh $@

$(TARGET).cia: $(TARGET).elf $(CIA_SMDH) $(CIA_ROMFS) $(CIA_DIR)/t2k.rsf \
               $(CIA_BANNER_DEP) $(CIA_DIR)/banner.wav
	@echo "TOOL resolving makerom + bannertool"
	@eval `$(CURDIR)/tools/get_cia_tools.sh` && \
	  echo "BNR  $(CIA_BANNER)" && \
	  $$BANNERTOOL makebanner $(CIA_BANNER_ARG) -a $(CIA_DIR)/banner.wav \
	                          -o $(CIA_BANNER) >/dev/null && \
	  echo "CIA  $@" && \
	  $$MAKEROM -f cia -o $@ -elf $(TARGET).elf -rsf $(CIA_DIR)/t2k.rsf \
	            -icon $(CIA_SMDH) -banner $(CIA_BANNER) -target t -exefslogo \
	            -DAPP_TITLE="$(APP_TITLE)" \
	            -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
	            -DAPP_UNIQUE_ID=$(APP_UNIQUE_ID) \
	            -DAPP_ROMFS=$(CIA_ROMFS)
	@ls -la $@

-include $(OFILES:.o=.d)

clean:
	@echo "CLEAN"
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).cia $(TARGET).map

# --- Scoped float-literal discipline (perf: hot per-frame geometry builders only) ---
# -fsingle-precision-constant kills implicit float->double->float promotion from
# unsuffixed FP literals in these hot per-vertex geometry builders. It is a
# STANDING GUARD, not a fix for anything in them today: their own per-vertex math
# is uniformly f-suffixed (grid textureLevel's *0.08f/*0.09f have been since the
# file's first commit -- an earlier version of this comment cited them as the
# motivating unsuffixed example, and they never were). What it buys is that a
# later unsuffixed literal cannot quietly reintroduce double math on the hot path.
# NB it is not inert: it also bites the double-typed ts::phase helpers (phase.h
# fromRadPerMs/fromPiPerMs), so do not extend it to more TUs casually.
# Scoped to these 3 TUs ONLY -- it is deliberately NOT global:
# textures.cpp (boot-time procedural-texture DSL) and game logic legitimately mix
# double vars with literals (std::max(double, 1e-10)) and must keep double semantics.
$(BUILD)/grid_geometry.o:   CXXFLAGS += -fsingle-precision-constant
$(BUILD)/line_geometry.o:   CXXFLAGS += -fsingle-precision-constant
$(BUILD)/entity_geometry.o: CXXFLAGS += -fsingle-precision-constant
