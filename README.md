# T2K: Psychedelic Tube Shooter
*A stereoscopic-native reimagining of Tempest 2000 for the Nintendo 3DS and stereoscopic displays. Inspired by sacred geometry psychedelia.*

![T2K banner](banner/t2k_composed.png)

Tempest 2000 has always been about depth: a web opening away from you, lanes rushing up the tube, everything lit like neon. T2K rebuilds that feeling for the 3DS's glasses-free 3D screen. It was designed stereoscopic from the beginning, so the geometry, the particles and the effects all assume two eyes — and will add a third one later.

The bottom screen is a full jukebox with music from across the Tempest family.

## The look
T2K takes its visual direction from sacred geometry psychedelia: symmetry, repetition, and glowing geometry that seems to unfold as you travel through it. An audio-reactive star tunnel provides a feeling of synesthesia as it pulses with the music.

## Stereoscopic native
T2K wasn't built in 2D and converted. The web, the plasma, the wireframe glow, every explosion, every particle and feedback loop was meticulously tuned for stereoscopic 3D from the start, to make full use of 3D displays. Each element — your shots, the blaster, the HUD text included — is projected separately for each eye, so each one carries its own parallax and sits at its own depth. There's no fake offset and no cardboard sprites. The tube opens out of the screen while the far lanes recede behind it.

## A full jukebox on the second screen
The bottom screen isn't a static menu. It's a full jukebox, and it's one of the biggest features in T2K. It plays music from across the Tempest line and its tube-shooter kin: Tempest 2000, Tempest 3000, Tempest 4000, TxK, and Space Giraffe. Play an entire album or pick individual tracks, and let the music advance with the levels so the soundtrack moves with you through the game.

## Sound that drives the picture
- Tracker (MOD) music and effects — a clean-room ProTracker replayer streamed as mono S16 @ 44.1 kHz through ndsp on a worker thread pinned to a spare CPU core.
- A real-time audio analyzer feeds the visuals: an audio-reactive star tunnel, a subtle bass-triggered dip in the web color, and a level select that moves with the beat. The FFT analyzer piggybacks on samples the DSP-ADPCM decoder already computes per-frame, so it costs effectively nothing. A photosensitivity safeguard limits how strongly and how quickly the visuals can pulse.
- The full soundtrack is bundled into the shipped build's romfs (multiple albums, DSP-compressed).

## The game
- **100 unique levels,** each a hand-tuned web that the engine extrudes into true 3D.
- **The full arcade roster:** Flipper, Tanker and its variants, Spiker, Fuseball, Pulsar, Mirror, Adroid, and Reflector.
- **Powerups and bonuses.** An eight-slot ladder covers laser, jump, tremor, droid, and superzapper with a warp token, plus surprises. Warp bonus rounds are in too.
- **Camera.** Two modes (Classic and Auto-framing, toggled with SELECT), an adjustable field of view, and C-Stick zoom on New 3DS.

## Controls

| Action | Button |
|---|---|
| Move claw | D-Pad ← / → or Circle Pad |
| Shoot | **A** |
| Jump | **B** |
| Tremor (web pulse) | **X** |
| Superzapper | **Y** |
| Pause / menu | **START** |
| Cycle camera viewpoint | **SELECT** |
| FOV zoom | **C-Stick** (New 3DS) |
| Music / jukebox deck | **Touch** (bottom screen) |

SELECT is reserved for the camera, so it never triggers a game action. The bottom screen stays dark until you tap it, and that tap wakes the jukebox.

## Shared technology
The same game runs on both targets because the shared core is built to constraints the 3DS forced.
- **One renderer seam, two GPUs.** A single flat C-API render seam (`t2k_core/src/rendering/render.h`): free functions every backend implements identically, GPU objects crossing as opaque `void*` handles. Shared game code never sees a `GLuint` or a `C3D_Tex`. One backend per target tree, selected at build time — no virtual `IRenderer`, no runtime cost.
- **"C with classes."** Data-oriented aggregate structs plus free functions — no virtual dispatch, no RTTI, no exceptions in the hot paths — so the same simulation compiles to a desktop Vulkan build and a 268 MHz handheld without `#ifdef`-ing away gameplay.
- **Shared audio core.** A clean-room ProTracker MOD replayer, a DSP-ADPCM decoder, the FFT analyser, and the SFX bank live in `t2k_core/src/audio/`; each target only supplies the output sink.
- **Exception-free, allocation-disciplined.** `-fno-exceptions -fno-rtti` clean throughout. JSON config/highscores use the exception-free nlohmann API — no `try/catch`. No STL heap growth in per-frame / per-entity / per-particle loops; fixed static arrays sized from `constants.h`. Packed/asset reads routed through an unaligned-access shim for ARM11.

## 3DS
The 3DS is the performance target and the reference for what the game contains. The hardware's limits are the reason the engine is shaped the way it is.

### PICA200 graphics engineering
The 3DS GPU has no programmable pixel shader, only six fixed-function combiner stages. T2K uses all six.
- **Six combiner stages at once.** The deepest surface pass chains two textures, tint, distance fog, and additive glow through the GPU's TEV stages.
- **Fog in the combiner.** Distance fade rides on vertex alpha, so there's no extra post pass.
- **Glow without a line primitive.** The PICA200 can't draw lines, so the tube's edge glow is built from billboard quads traced along every edge, per eye, with depth falloff.
- **Free anti-aliasing.** The frame renders oversized and the display transfer downsamples it for a clean 2x supersample.
- **Procedural textures.** A texture DSL executes on the CPU to RGBA8, then Morton/Z-order tile-swizzles into PICA ABGR8 textures. Level textures are generated on the fly, and a background worker on the second core prepares them ahead of time so levels load quickly.

### GPU vertex animation (CPU → GPU offload)
- The undulating **web "tremor" wave** used to be recomputed on the CPU every frame (a `sin` over 4,480 vertices, plus re-uploading all their positions). It's now a **static base mesh uploaded once per level** plus a **grid-only picasso vertex shader** that applies the wave displacement (`pos += normal · sin(phase) · tremor · √depth`) and distance fog on the **GPU vertex unit**.
- PICA has no `sin` instruction, so the shader uses a **polynomial approximation** over `[-π, π]` after range reduction (max error 0.0011, exact at 0/±π/2/±π — sub-pixel).
- The wave is bound **only** for the grid draw and restored afterward, so entities/HUD never wave.

### Measured, honest performance work
- On-device profiling (`svcGetSystemTick`, one line / 60 frames to an FTP-pullable SD log) split into **CPU build vs. GPU wait**, with the build hoisted **before `C3D_FrameBegin(SYNCDRAW)`** so it overlaps the previous frame's GPU work (software pipelining).
- Key finding, proven by A/B: on a many-pass stereo frame the PICA200 is **draw-call / state-bound, not fill-bound** — halving the supersample barely moved GPU time, so supersample AA is ~free.
- **Per-model bottleneck asymmetry:** New 3DS (804 MHz, +L2 enabled) is GPU-bound; the OG 3DS (~268 MHz) is CPU-bound — so the GPU wave offload is *headroom* on New and *decisive* on OG.
- Plasma trails composited at low res (32×32) to collapse two full-screen additive passes into one.

### Camera & options
- **Auto-framing camera.** Camera distance and offset are derived from the web's measured bounding box, so every playfield shape frames itself instead of inheriting constants tuned for one level. Both modes run through one eased smoother, so switching crossfades rather than snapping.
- **Live Field-of-View control** — a graphics-menu slider (40–75°) and the New 3DS C-stick as a zoom, both applying in real time and persisted to the SD config.
- Web glow, texture brightness, background, and audio album are all adjustable, and your settings are saved — on-SD JSON config under `sdmc:/3ds/t2k/`, with persisted high scores.

### Hard-won PICA200 / 3DS gotchas

Documented in full in the shared port-docs vault; the short list, because each one cost real time:

- **NDC `z = 0` is the FAR clip plane.** PICA depth is `[-1, 0]`, so a CPU-projected billboard written at `z = 0` sits exactly on the far plane: trivially-accepted triangles draw, but anything the clipper must actually *clip* is discarded. Symptom is geometry vanishing only when it straddles the screen edge. Always write a `z` strictly inside `(-1, 0)`.
- **No guard band, and `float24` vertex coords** (16-bit mantissa). Clip the billboard *centreline* to a small NDC guard rect, then expand to quads — expanding first loses thickness at the seam, and large NDC magnitudes quantize thin quads to zero area.
- **`hidCstickRead` is `irrstCstickRead`** — it needs `irrstInit()`, which libctru does not call for you. It compiles, links, and reads nothing.
- **The emulator hides some of these.** Citra/Mandarine computes above `float24` precision and does not run PICA geometry shaders, so a class of bug is emulator-clean and hardware-only. A looked-at frame on real hardware is the only pass.
- **Measure at the CPU/GPU boundary before editing.** Plausible theories for a vanishing-geometry bug can all be wrong; a host-side harness linking the real engine data plus the real citro3d matrix semantics is what localises it in one pass.

## PC / VR
The desktop build is the correctness oracle and the arcade/VR target. It runs the exact same simulation as the 3DS — the 3DS is the reference for what the game contains; the PC is free in how it draws it (see the Aesthetic Contract in `DOCTRINE.md`).
- **SDL2 + Vulkan 1.3 renderer** (`t2k_pc/src/rendering/renderer_vk.cpp` + `vk_*.cpp`, GLSL in `shaders/`): SDF vector lines and a bloom pyramid give the glow a softness the PICA200 can only fake with quads.
- **Live procedural textures in fragment shaders.** The same texture-DSL semantics that run on the CPU for the 3DS seethe per-pixel on desktop instead.
- **Multiview stereo for OpenXR.** The engine was built stereoscopic-native for the 3DS, so the VR path projects the same per-eye geometry through a headset instead of retrofitting a 2D game.
- **Byte-level oracle.** Host harnesses link the real engine data and the real matrix semantics, which is what localised hardware-only bugs (see the gotchas above) in one pass.

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
| `soundtracks/` | The soundtrack pipeline: fetch → decode → DSP-ADPCM encode → album map, pinned by `track_map.json` |
| `docs/` | Design notes, validation write-ups, and the engine doctrine (`DOCTRINE.md`) — **local-only, not in the clone** |
| `tools/` | Repo-level gate scripts + the vendored DSP-ADPCM encoder (`tools/vendor/`) |

## Building from scratch

One command, from a bare clone:

```
./build.sh
```

That runs the whole chain and leaves `t2k_3ds/t2k.cia` plus its SHA-256:

| # | Stage | What happens |
|---|---|---|
| 1 | **bootstrap** | checks `git` / `make` / `python3` / `ffmpeg` and devkitARM; installs the pinned Python build deps (`requirements-build.txt`) into the ambient interpreter if it already has them, otherwise into a project-local `.venv`; compiles the vendored DSP-ADPCM encoder into `tools/bin/dspadpcm` |
| 2 | **fetch** | downloads the source FLACs from KHInsider into `soundtracks/<album>/` — resumable, existing files are skipped |
| 3 | **audio** | `ffmpeg` → 32 kHz mono PCM → `dspadpcm` → `soundtracks/dsp/*.dsp`; copies the MOD album through unchanged; writes `dsp/albums.json`; **hash-checks every output** against `soundtracks/track_map.json`; stages the pool to `data/music/` |
| 4 | **cia** | `make -C t2k_3ds cia` → `t2k_3ds/t2k.cia` |
| 5 | **report** | artifact path, size, SHA-256 |

If the fetch fails (Cloudflare, offline), the build **still finishes** as a
pool-less CIA rather than leaving you with nothing — the game plays either way,
because the MOD chiptunes and the album manifest are embedded in the binary.

### Targets

| Command | Does |
|---|---|
| `./build.sh` | bootstrap → fetch → audio → CIA |
| `./build.sh bootstrap` | deps + Python env + the vendored encoder, nothing else |
| `./build.sh fetch` | download the source FLACs only |
| `./build.sh audio` | encode the DSP pool + MOD copy + `albums.json` + stage `data/music/` |
| `./build.sh cia` | package the CIA from the current tree (dirty tree OK) |
| `./build.sh production` | the strict, traceable CIA — refuses a dirty tree, always cleans, verifies banner / exheader / SMDH from the built bytes |
| `./build.sh pc` | the desktop oracle (SDL2 + Vulkan 1.3) |
| `./build.sh verify` | hash the built pool against `soundtracks/track_map.json` |
| `./build.sh check` | the repo gate suite (VFP hot-path contract, ARM11 math audit, R11 screen, level/demo audits) |
| `./build.sh clean` | generated pool + staging + build trees (keeps downloaded FLACs and `.venv`) |

### Flags

| Flag | Effect |
|---|---|
| `--no-audio` | build a CIA that provably carries **no** third-party audio (`T2K_MUSIC_POOL=none`) — ~2 MB, for licensing submissions or handing to a third party |
| `--skip-fetch` | never hit the network; fail if the source FLACs are absent |
| `--venv` | force a project-local `.venv` even if the system interpreter already has the deps (keeps conda/system site-packages out of the build) |
| `--force` | re-encode every track even when it already matches the pinned hash — proves the map still holds under a changed encoder/ffmpeg |

`./build.sh --help` prints this too.

### Prerequisites

- **System:** `git`, `make`, `python3` (3.10+), `ffmpeg`.
- **3DS build:** devkitPro — `devkitARM`, `libctru`, `citro3d`, `picasso`. Read from
  `$DEVKITPRO` / `$DEVKITARM`, defaulting to `/opt/devkitpro`.
- **Python:** `beautifulsoup4`, `curl_cffi`, `pyflakes` — installed automatically
  by the bootstrap stage from `requirements-build.txt`.
- **Desktop build only:** CMake ≥ 3.16, SDL2 + SDL2_mixer dev packages, `glslang`
  on PATH (or `$VULKAN_SDK/bin`), and a Vulkan 1.3 driver at runtime.

### Where the audio comes from

The soundtrack is copyrighted, so **neither the `.dsp` pool nor the source FLACs
are in this repository**. What *is* here is `soundtracks/track_map.json`: every
deployable file's canonical name, album, album order and SHA-256, derived by
byte-exact encode match against the reference pool. The encoder that reproduces
those bytes is vendored at `tools/vendor/gc-dspadpcm-encode` (MIT). So the pool
is reproducible from source without ever being committed, and "correct file,
correct name, correct order" is verified by hash rather than trusted.

The five source albums are fetched from KHInsider into `soundtracks/`:

| Album | Folder | KHInsider source |
|---|---|---|
| Tempest 2000 | `tempest2000_soundtrack/` | `tempest-2000-the-soundtrack-1995` (12) |
| Tempest 3000 | `tempest3000_soundtrack/` | `tempest-3000-nuon-gamerip-2000` (19) |
| Tempest 4000 | `tempest4000_soundtrack/` | `tempest-4000-gamerip` (144 → deduped) |
| TxK | `TxK/` | `txk-ps-vita-gamerip-2014` (21) |
| Space Giraffe | `Space_Giraffe/` | `space-giraffe-windows-gamerip-2009` (4) |

The Tempest 2000 MOD album is **not** fetched — `soundtracks/mod/*.mod` is
tracked, streams natively, and is copied (renamed `t2k-N.mod` → `t2000-N.mod`)
rather than re-encoded.

KHInsider sits behind Cloudflare, which answers `cf-mitigated: challenge` on a
random fraction of requests (~42% pass measured 2026-09-27) — a per-request
lottery, not a per-URL block. The downloader rebuilds its TLS + cookie session on
every retry, which is what demonstrably clears it. If it throttles hard for a
long stretch, solve the challenge in a browser and pass the clearance through:

```
KHINSIDER_COOKIE='cf_clearance=…' KHINSIDER_UA='<the UA that solved it>' ./build.sh fetch
```

`cf_clearance` is bound to the User-Agent that solved the challenge, so the two
must match.

### Gates

`./build.sh check` runs the repo's verification suite (both target builds, the VFP
hot-path contract, the ARM11 math audit, the R11 integer-div screen, level +
warp-window + demo audits, the `@vfp-exempt` pin floor, pyflakes).

> The gate suite lives in `tools/runner/`, which is **local-only** (gitignored), as
> is `docs/`. A bare clone therefore has no gate suite and `./build.sh check`
> reports that it skipped rather than failing on a missing file. Everything needed
> to *build* the game is tracked; the gates and the docs tree are the working
> material of the primary checkout.

### Installing on hardware

Copy `t2k_3ds/t2k.cia` to the SD card and install it with your installer of
choice (FBI, DreamTool, …). The soundtrack lives in the CIA's **romfs**, read in
place — nothing needs to be copied to `sdmc:` besides the CIA itself. The first
boot seeds `sdmc:/3ds/t2k/t2k_config.json` from the bundled template; that file
is yours to edit (controls, audio source, render toggles).

Anything leaving this machine should go through `./build.sh production`, not a
bare `cia`: it refuses a dirty tree, always builds from clean, and re-reads the
shipped bytes to verify the HOME-menu identity, the 16-bit stereo banner audio and
the DSP-RAM exheader mapping — the three traps that build green and fail silently
on a console.

### Desktop (correctness oracle)

```
./build.sh pc      # -> t2k_pc/build/t2k
```

### Make targets

`build.sh` is the front door; the underlying `make` targets still work:
`make pc`, `make 3ds`, `make cia`, `make production`, `make check`, `make clean`.
`make -C t2k_3ds clean` is mandatory when flipping any `-D` build flag.

## Ports & platform gating

- **API gate** `__3DS__` / `PLATFORM_3DS` — SDL2+GL vs libctru+Citro3D.
- **CPU gate** `ARM` / `PLATFORM_ARM` — unaligned-access shims, ARMv6K vs ARMv7 ops.
- Kept independent, and gameplay logic is never `#ifdef`'d out to make a platform build pass.

## Contributing

PR requests are welcome — so long as they adhere to the [`AGENTS.md`](AGENTS.md) contract. That file is the enforceable ARM11/VFP math contract (hot-path rules, FPSCR requirements, exemption grammar), and it exists so the code stays oriented around the capabilities of the ARM11 at all times. A change that violates the contract isn't merged, however good it looks on desktop.

## License

The **TSEngine** codebase (all C++ sources, build files, shaders, and tooling) is
released under the **MIT License** — see [`LICENSE`](LICENSE).

**Game assets are separate.** Bundled audio and sound effects that reproduce or
were derived from commercially released games remain under their original
copyright and are **not** covered by the MIT grant. They are tracked in this
repository so the project clones and builds as intended, but they are not
licensed to you by MIT — remove them and supply your own audio, or hold the
separate rights to anything you redistribute. See [`NOTICE.txt`](NOTICE.txt)
for the full breakdown of asset categories and the vendored third-party library
licenses.
