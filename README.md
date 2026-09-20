# Tsunami 2010 — Free the reference build → C++ port (PC + Nintendo 3DS)

A from-scratch C++ port of the Free the reference build shareware game **Tsunami 2010** (a *Tempest 2000*-style
procedural tube shooter), engineered to run on **two very different machines behind a single
codebase**:

- **PC / x86-64** — SDL2 + **Vulkan 1.3** (SDF vector lines, a bloom pyramid, live-seething
  procedural textures in fragment shaders, multiview stereo for OpenXR; the arcade/VR target).
- **Nintendo 3DS / ARM11** — libctru + **Citro3D / PICA200** (the performance target).

The game is built as **"C with classes"** — data-oriented aggregate structs + free functions,
no virtual dispatch / RTTI / exceptions in the hot paths — so the same logic compiles to a
desktop Vulkan build *and* a 268 MHz handheld without `#ifdef`-ing away gameplay.

---

## Highlights (the stuff worth bragging about)

### Reverse-engineering the shipped game as ground truth
- The recovered the reference build source is an **earlier build** than the shipped `the shipped PC port.exe`. When they
  disagree, **the binary wins.** The `.exe` is decompiled with **Ghidra** (headless `GhidraScript`
  runs, image base `0x400000`) and used as the deciding oracle.
- The **web geometry bug** ("stretched top segment / you can circle a broken web") was traced this
  way: the shipped exe builds each lane as a **unit vector** (`dy = grid_y/127` as the *sine* of the
  lane angle, `dx = ±√(1−dy²)` the cosine), which is what makes "round" levels close into clean
  polygons. The recovered the reference build used a constant `dx = ±1` (an alpha bug). The port follows the exe.
- Exe-verified draw counts, player-clamp behavior, and lane vectors — cross-checked against the
  x86 decompile, not trusted from the readable-but-wrong the reference build.

### One renderer seam, two GPUs
- A single flat **C-API render seam** (`rendering/render.h`): free functions every backend
  implements identically, GPU objects crossing as opaque `void*` handles. Shared game code never
  sees a `GLuint` or a `C3D_Tex`.
- **One backend per target tree, exactly one compiled in** (`t2k_pc/src/rendering/renderer_vk.cpp`
  / `t2k_3ds/src/rendering/renderer_c3d.cpp`), selected at **build time** — no virtual `IRenderer`,
  no runtime cost. Geometry comes from shared builders in `t2k_core/src/rendering/`; a backend file
  only submits. The 3DS build is the reference for what the game contains; the PC is free in how it
  draws it (see the Aesthetic Contract in `DOCTRINE.md`).

### PICA200 / Citro3D graphics engineering
- **Rotated top-screen target** (the panel is physically 240×400, rendered 90° rotated) with the
  NDC depth remap `[-1,1] → [-1,0]` folded into the projection.
- **Procedural texture DSL** (the original `Tex*.inc` language) executed on the CPU to RGBA8, then
  **Morton/Z-order tile-swizzled** and uploaded as PICA ABGR8 textures.
- **Stereoscopic 3D** — genuine per-eye projection (the web, plasma, and glow all carry honest
  parallax depth), not a fake 2D offset.
- **Fixed-function TEV** shading (6 stages; PICA has *no* programmable fragment shader) driving
  multi-texture web passes, distance fog via the TEV combiner, and additive glow.
- **Wireframe glow without a line primitive** — PICA can't draw lines, so the web's border glow is
  emitted as CPU-built billboard quads traced along the tube edges, per-eye, with depth falloff.
- **Hardware anti-aliasing for free** — render into an oversized target and let the display-transfer
  PPF **box-downscale** it (2× horizontal supersample) on the way to the screen.

### GPU vertex-animation (CPU → GPU offload)
- The undulating **web "tremor" wave** used to be recomputed on the CPU every frame (a `sin` over
  4,480 vertices, plus re-uploading all their positions). It's now a **static base mesh uploaded
  once per level** + a **grid-only picasso vertex shader** that applies the wave displacement
  (`pos += normal · sin(phase) · tremor · √depth`) and distance fog on the **GPU vertex unit**.
- PICA has no `sin` instruction, so the shader uses **Nick's polynomial approximation** over
  `[-π, π]` after range reduction (max error 0.0011, exact at 0/±π/2/±π — sub-pixel).
- The wave is bound *only* for the grid draw and restored afterward, so entities/HUD never wave.
  Gated so the PC/GL oracle stays byte-identical.

### Measured, honest performance work
- On-device profiling (`svcGetSystemTick`, one line / 60 frames to an FTP-pullable SD log) split
  into **CPU build vs. GPU wait**, with the build hoisted **before `C3D_FrameBegin(SYNCDRAW)`** so
  it overlaps the previous frame's GPU work (software pipelining).
- Key finding, proven by A/B: on a many-pass stereo frame the PICA200 is **draw-call / state-bound,
  not fill-bound** — halving the supersample barely moved GPU time, so supersample AA is ~free.
- **Per-model bottleneck asymmetry:** New 3DS (804 MHz, +L2 enabled) is GPU-bound; the OG 3DS
  (~268 MHz) is CPU-bound — so the GPU wave offload is *headroom* on New and *decisive* on OG.
- Plasma trails composited at low res (32×32) to collapse two full-screen additive passes into one.

### Configurable views & camera framing (beyond the original)

The port is faithful by default but is not limited to the original's presentation:

- **Live Field-of-View control** — a graphics-menu slider (40–75°) *and* the **New 3DS C-stick**
  as a zoom, both applying in real time and persisted to the SD config. (The shipped original hard-codes
  45°; Typhoon 2001, a sibling remake, likewise drives FOV from a runtime setting rather than a constant.)
- **Two camera modes**, toggled live with **SELECT**:
  - *Classic* — bit-exact to the shipped binary (`camZ 7.5`, `camY −1.25`, `0.6/0.4` follow blend,
    all recovered from the decompile).
  - *Auto-framing* — camera distance and offset **derived from the web's measured bounding box**,
    so every playfield shape frames itself instead of inheriting constants tuned for one level.
    A single-radius heuristic over-pulls on elongated webs by ~2.4×; a per-axis fit (accounting for
    which world axis maps to which screen axis after the 90° screen rotation) does not.
  - Both modes run through **one eased smoother**, so switching crossfades rather than snapping, and
    all camera terms ease at the *same* rates as the original's follow (`0.025`/`0.03`) — easing a
    derived term faster than the follow makes them fight and reads as jitter.
- **Honest stereoscopic 3D** — the web, plasma, wireframe glow, and the line entities
  (shots/blaster/explosions) are each projected **per eye**. CPU-projected geometry is built into
  per-eye buffer regions; sharing one buffer silently gives both eyes the same projection, which looks
  like shots firing into the neighbouring lane.

### Audio, clean-room
- A **clean-room ProTracker MOD replayer** ported from the original's x86/MMX assembly, streamed as
  mono S16 @ 44.1 kHz through **ndsp on a worker thread pinned to a spare CPU core**.
- The Mandelbrot fractal background is **gone** (see DOCTRINE.md "Intentional deviations") — the 3DS
  core it used to occupy now runs an **audio-reactive FFT analyzer** driving the starfield background
  and a bass-triggered web colour duck, at effectively no extra cost: the analyzer piggybacks on
  samples the DSP-ADPCM decoder already computes per-frame.

### Exception-free, allocation-disciplined
- `-fno-exceptions -fno-rtti` clean throughout. JSON config/highscores use the **exception-free
  nlohmann API** (`parse(..., allow_exceptions=false)` + type-guarded accessors) — no `try/catch`.
- No STL heap growth in per-frame / per-entity / per-particle loops; fixed static arrays sized from
  `constants.h`. Packed/asset reads routed through an unaligned-access shim for ARM11.

### Player-facing polish
- On-SD JSON config with **remappable controls**, audio-source and rendering toggles (see-through
  web, glow, plasma, detail), and persisted high scores — all under `sdmc:/3ds/the shipped PC port/`.

---

## Hard-won PICA200 / 3DS gotchas

Documented in full in the shared port-docs vault; the short list, because each one cost real time:

- **NDC `z = 0` is the FAR clip plane.** PICA depth is `[-1, 0]`, so a CPU-projected billboard written
  at `z = 0` sits exactly on the far plane: trivially-accepted triangles draw, but anything the clipper
  must actually *clip* is discarded. Symptom is geometry vanishing only when it straddles the screen
  edge. Always write a `z` strictly inside `(-1, 0)`.
- **No guard band, and `float24` vertex coords** (16-bit mantissa). Clip the billboard *centreline* to a
  small NDC guard rect, then expand to quads — expanding first loses thickness at the seam, and large
  NDC magnitudes quantize thin quads to zero area.
- **`hidCstickRead` is `irrstCstickRead`** — it needs `irrstInit()`, which libctru does not call for
  you. It compiles, links, and reads nothing.
- **The emulator hides some of these.** Citra/Mandarine computes above `float24` precision and does not
  run PICA geometry shaders, so a class of bug is emulator-clean and hardware-only. A looked-at frame on
  real hardware is the only pass.
- **Measure at the CPU/GPU boundary before editing.** Five plausible theories for one vanishing-geometry
  bug were all wrong; a host-side harness linking the real engine data plus the real citro3d matrix
  semantics is what localised it in one pass.

## Repository layout

| Path | What |
|---|---|
| `t2k_core/src/game/` | Core simulation (engine, enemies, weapons, collision, player, camera) — platform-agnostic, compiled into both targets |
| `t2k_core/src/rendering/` | The `render.h` / `font.h` seam headers + the shared geometry builders both backends consume |
| `t2k_core/src/audio/` | Shared audio core: ProTracker replayer, DSP-ADPCM decoder, FFT analyser, SFX bank |
| `t2k_core/src/data/` | Save/load (exception-free JSON), level + enemy data; `t2k_core/data/levels.json` is THE level list |
| `t2k_core/src/ui/` | Seam-only menus / level select / touch panel (no SDL, no libctru) |
| `t2k_core/tools/` | Level generator + verifier, harnesses, bin2h |
| `t2k_3ds/` | The 3DS target: `Makefile`, `src/platform_3ds/` entry point, `src/rendering/renderer_c3d.cpp` (Citro3D), ndsp audio sinks, `shaders/*.v.pica` |
| `t2k_pc/` | The desktop target: `CMakeLists.txt`, `src/main.cpp` (SDL2), the Vulkan 1.3 renderer (`renderer_vk.cpp` + `vk_*.cpp`, GLSL in `shaders/`), SDL audio sinks |
| `the reference source/` | Authoritative original source + the shipped `the shipped PC port.exe` (RE oracle) |
| `the shipped PC port_py/` | Completed Python (pygame + PyOpenGL) reference port |

## Building

**Desktop (correctness oracle):**
```
make pc            # -> t2k_pc/build/t2k   (cmake -S t2k_pc -B t2k_pc/build under the hood)
                   #    needs: SDL2 + SDL2_mixer dev packages, glslang (glslang or
                   #    glslangValidator on PATH / $VULKAN_SDK/bin / ~/.local/opt/glslang/bin),
                   #    a Vulkan 1.3 driver at runtime (volk dlopens libvulkan.so.1; the
                   #    headers, volk and the OpenXR headers are vendored in t2k_pc/third_party)
```

**Nintendo 3DS** (needs devkitPro: devkitARM, libctru, citro3d, picasso/tex3ds):
```
make 3ds           # -> t2k_3ds/t2k.3dsx  (make -C t2k_3ds under the hood)
```
> `make -C t2k_3ds clean` is mandatory when flipping any `-D` build flag.

## Ports & platform gating

- **API gate** `__3DS__` / `PLATFORM_3DS` — SDL2+GL vs libctru+Citro3D.
- **CPU gate** `ARM` / `PLATFORM_ARM` — unaligned-access shims, ARMv6K vs ARMv7 ops.
- Kept independent, and gameplay logic is never `#ifdef`'d out to make a platform build pass.
