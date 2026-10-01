# T2K — agent contract

Design language, fidelity ladder, and the rest of the doctrine live in
`docs/DOCTRINE.md`. This file is the **enforceable ARM11 math contract** that the
2026-09-06 OG-profile playtest closed. Do not "simplify" it back to libm,
double, or float control-flow in hot loops.

The 3DS (ARM11 MPCore, VFP11 / VFPv2, 268 MHz with L2 off) is the performance
target and the reference for what the game contains. OG profile
(`og_profile: true` in `sdmc:/3ds/t2k/t2k_config.json`) is how a New 3DS
measures that target. A New-clocked run is not an OG pass.

---

## 1. What the ARM11 actually taxes

Raw float arithmetic is not the problem. Four things are:

1. **The coprocessor boundary.** Moving a value between the ARM integer
   register file and the VFP register file costs 20+ cycles (`vmov` / `vmrs` /
   `vcvt`). A per-iteration round trip dominates the loop.
2. **Trap-to-support-code on denormals.** Decay loops generate denormals by
   construction. Without Flush-to-Zero they stall hard and the stall is
   *irregular* — that is frame-pacing jank, not a smooth FPS drop.
3. **Register file size.** 32 single-precision registers. Doubles consume two.
   Spills are memory traffic on a 16 KB L1.
4. **Code footprint.** 16 KB L1 instruction cache, and L2 is OFF in an OG
   profile. This is what inlining excess and template bloat actually cost —
   not the arithmetic. It is also the only honest ARM11 argument for the
   template restriction in §10; the "templates are slow" framing is false.

This contract targets 1–3 with §§3–4. It does **not** prohibit floating-point
math, and it does **not** prohibit mixing int and float in the same function.
Item 4 is named for reviewers: nothing here gates it.

Full rule text, exemption grammar, standing exemptions, and the report schema:
`tools/runner/contracts/vfp_hotpath.md`. The corpus of known hot functions is
`tools/runner/contracts/hotpath_corpus.json`. Gates:
`python3 -m tools.runner.gates_vfp` (via the swarm runner). Trig/fixed-point
audit: `python3 tools/runner/audit_arm11_math.py`.

---

## 2. HOT PATH

A function or loop body is HOT PATH if it:

- runs per-frame at ≥ 1× frame count AND is in the top 15 of a captured
  profile, OR
- runs ≥ 256 times per frame regardless of rank, OR
- is annotated `/* @hotpath */`.

**THE CORPUS IS DERIVED FROM A PROFILE, NEVER FROM MEMORY.** "Top 15 of a
captured profile" is only meaningful against a CURRENT capture: code that was
cold when the list was written becomes hot when a caller changes, and a
hand-maintained list of hot functions silently stops covering it. Regenerate the
corpus from the profile you are actually optimising against, and treat a
function that entered the top 15 since the last capture as newly in scope. This
is the answer to "the hot/cold boundary is a judgement call" -- it is a
measurement, and it is only as good as its last measurement.

Nothing in §§3–5 applies to code that is not HOT PATH. Init, level load, menu
logic, texture generation, parse, and teardown are COLD. Do not "clean up"
cold code to satisfy these rules.

---

## 3. MUST rules (build-blocking)

### R1 — FPSCR FZ + DN on every float thread

Flush-to-Zero (bit 24) and Default-NaN (bit 25) MUST be set in `FPSCR` on
every thread that executes float math, at thread entry, **before any float
work**. FPSCR is per-thread; the main thread does not cover workers.

Shipped helpers (do not delete, do not `#ifdef` out, do not skip the
read-back on main):

| Thread | Helper | Call site |
|---|---|---|
| main | `vfp_set_fz_dn()` in `t2k_3ds/src/platform_3ds/main_3ds.cpp` | start of `main`, before `gfxInitDefault`; failure `return 1` immediately, never `goto quit` |
| music worker | `vfp_thread_fz_dn()` in `t2k_3ds/src/audio/music_3ds.cpp` | first line of `musicThread` |
| SFX worker | `vfp_thread_fz_dn()` in `t2k_3ds/src/audio/sfx_3ds.cpp` | SFX thread entry |
| renderer worker | `vfp_thread_fz_dn()` in `t2k_3ds/src/rendering/renderer_c3d.cpp` | renderer thread entry |
| shared audio workers | `vfp_thread_fz_dn()` in `t2k_core/src/audio/audio_thread.h` | 3DS branch only |

Every helper and call site MUST keep the VFPU rationale comment (denormals
stall VFP11 unless FZ is set; decay loops generate them; FPSCR is
per-thread). Uncommented FPSCR asm is a MUST finding.

A new 3DS worker thread that touches floats MUST get the same helper at
entry. Copy the existing comment; do not invent a second protocol.

### R2 — No float→int conversion in a hot loop body

No `(int)x`, `vcvt`, `lroundf`, or array-index-from-float inside the loop.
Hoist it, keep the domain integer, or pin an exemption.

### R3 — No float comparison driving control flow in a hot loop

A `vcmp`/`vmrs` that feeds a branch is a register-file round trip. Gameplay
swept windows (shot/enemy/player z-tests, spike lethality) are the hit set —
integerizing them changes the game. Pin them (`@vfp-exempt R3`); do not
"fix" them.

### R4 — No double precision in hot paths

`double` / `std::sin` (unsuffixed) / `(float)(M_PI * …)` promotions that
force a double multiply are forbidden. Use `float`, `f`-suffixed literals,
and `(float)M_PI * 0.00017f` not `(float)(M_PI * 0.00017)`. Perf
accumulators in `renderer_c3d.cpp` are `float` on purpose.

Gameplay trig that is **mechanics-gated DOUBLE** (`engine.cpp` spawn counts,
`weapons.cpp` tremor decay) is a standing exemption — a ULP shift there
flips `round()` spawn boundaries. Do not LUT it. Do not `float` it.

### R5 — Loop counters and indices MUST be integer

Unsigned induction where the value is a lattice id (`uint32_t j/k` salt math
in `rail_geometry.cpp`). Do not reintroduce `(uint32_t)j` casts inside those
loops.

### R6 — No libm transcendentals in hot paths

Presentation / camera / mesh / panel trig goes through `ts::fastSin` /
`fastCos` / `fastAtan2` in `t2k_core/src/game/math_lut.h`. The table is
**one path on both targets**. Do not resurrect `#if __3DS__` LUT / `#else`
libm — that split made host harnesses prove the desktop arithmetic and say
nothing about the 3DS.

Do not route through `__builtin_floorf` for range reduction. VFPv2 has no
float-floor; GCC emits `bl floorf` plus a VFP spill frame and the "LUT"
costs more than libm. The leaf `(int)` truncation + sign fixup in
`fastSin` is load-bearing. Do not "clean" it.

Call `mathLutInit()` from every new entry point (including host harnesses)
before any `fast*`. Uninitialized tables are zero; every trig call returns 0.

### R10 — Build flags are verified, not assumed

`t2k_3ds/Makefile` MUST contain:

```
ARCH := -march=armv6k -mtpcs-leaf-frame -mfloat-abi=hard -mtune=mpcore -mfpu=vfp
```

and `-fno-math-errno` (so GCC emits `vsqrt`/`vdiv` instead of errno-setting
libm). Do not add `-ffast-math`. Reciprocal-once hoists stay manual because
the compiler will not do them.

---

## 3b. TEMPERATURE-INDEPENDENT rules (apply in COLD paths too)

Everything in §3 is a COST argument and is therefore scoped to hot paths. These
two are DEFECT arguments -- they are wrong in cold code as well, for reasons
that have nothing to do with the VFP -- so they are the only rules in this file
that apply everywhere.

**They exist because widening §3 to all floats was considered and REJECTED**
(2026-09-08). The proposal was to scrutinise every float rather than only hot
ones. Against: §1's own premise is that raw float arithmetic is not the problem;
there are ~3,500 float declarations across the two trees against 227
`@vfp-exempt` pins today, so blanket scope is roughly a 15x annotation burden
whose overwhelming majority would be rubber-stamped -- and a rule exempted
thousands of times trains reflexive stamping, which is worse than no rule
because it looks like scrutiny. The defect that prompted the proposal (below)
would ALSO NOT HAVE BEEN CAUGHT by it, which is what settled the question: it
was not a float-usage bug. These two rules catch it; blanket float scope does
not.

### T1 — Solve, don't search

Do not iterate toward a value that has a closed form. If the condition is linear
(or otherwise directly invertible) in the unknown, invert it.

Earned on `ui/touch_panel.cpp`'s `fitName`, which shrank a text scale by
`use -= 0.0005f` in a loop until the string fit -- up to ten float comparisons
for an answer that is one divide, because the fit condition
`len * (THICK + 1.3) * use <= avail` is linear in `use`.

It is temperature-independent because it is not a cycle argument: a search where
a solution exists is harder to read, has an approximate answer, and hides its
termination condition in a step size. **Fixed point would not have fixed it** --
a fixed-point descending loop is equally wrong -- which is exactly why this is
its own rule rather than a float rule.

### T2 — No float comparison in a LOOP CONDITION, at any temperature

`while (x > limit) x -= step;` is a termination hazard before it is a
performance one: floating-point accumulation decides the iteration count, so the
loop can run one iteration more or fewer than intended, and with a small enough
step or an unlucky value it may not terminate at all. Use an integer iteration
count, or solve it (T1).

This is NARROWER than R3 on purpose. R3 covers float comparisons driving control
flow generally and stays hot-only, because a swept-window `if` is a legitimate
gameplay predicate that must not be integerised (see R3's own note). T2 is only
about the CONDITION OF A LOOP, where the comparison is choosing how many times
to run rather than what to do.

### The standing exemption for BOUNDED NORMALISATION — pin it, do not fix it

The tree was audited against T1/T2 when they were written (2026-09-08). It found
**nine sites, and every one is exempt.** They are all the same shape:

```c
while (d >  128.0f) d -= 256.0f;      // warp.cpp, a bonus-round angle
while (h >= 360.0)  h -= 360.0;       // textures.cpp, a hue wrap
```

A wrap like this HAS a closed form (`fmod`, or `x - k*floor(x/k + 0.5)`), so it
reads as a T1 violation, and it is a float loop condition, so it reads as T2.
Both readings are wrong here and acting on either would be a REGRESSION:

- the input is already near range, so it runs one or two iterations -- there is
  no search, and no termination hazard in practice;
- `warp.cpp` and `entity_geometry.cpp` are **ported gameplay math**, which
  DOCTRINE.md gates: a closed form is not bit-identical to repeated subtraction,
  and a ULP there moves bonus-round geometry;
- `textures.cpp` is the `Tex*.inc` DSL, which **defines asset output** -- the
  same change moves texture bytes.

So: `@vfp-exempt T1` / `@vfp-exempt T2` and move on. **The rule is for code being
WRITTEN, not a licence to rewrite ported math that already works.** This
paragraph exists because a rule without its exemption is an invitation, and the
nine sites above are exactly the ones a confident reader would "clean up".

---

## 4. SHOULD rules

- **R7** — Division and sqrt: VFPv2 has hardware `vdiv`/`vsqrt`. Prefer
  reciprocal-once outside the loop (`invHalfW` / `invHalfH` pattern) over
  per-element divides. Do not table them; Forsaken already learned
  fast-invsqrt is slower and wrong.
- **R8** — Fixed-point domains must not implicitly promote back to float
  mid-loop.
- **R9** — Stay inside the 32-S register file. Doubles and live ranges that
  force spills are the failure mode.
- **R11 — INTEGER division or modulo by a RUNTIME value in a hot loop.**
  Added 2026-09-07. **ARMv6 has no integer divide instruction at all**, so a
  runtime `%` or `/` on integers is a `bl __aeabi_idivmod` / `__aeabi_idiv` —
  tens of cycles *plus* a call barrier that spills the loop's live registers.
  This is categorically worse than the float division R7 flags, because VFPv2
  *does* have hardware `vdiv`. A **compile-time-constant** divisor is fine:
  GCC turns it into a magic-number multiply.
  - The idiom that costs most is `((x % n) + n) % n` — **two** calls — and it
    is nearly always wrapping a value that is at most one period out of range.
    Where that bound can be proven, `if (v < 0) v += n; else if (v >= n) v -= n;`
    is an **exact integer identity**, not an approximation.
  - Found this way and fixed: `grid_geometry.cpp lightenLevel` was paying two
    per innermost iteration of a 22×22 explosion kernel, for a value that never
    depended on the inner loop variable. Measured ~3,260 divide calls/frame at
    the mean and ~53,240 at 55 live explosions, in the `grid` stage that is
    ~10 ms of the OG frame. See
    `docs/validation/arm11-second-opinion-verdict-2026-09-07.md`.
  - **THE SCREEN NOW EXISTS**: `python3 tools/runner/audit_r11_idiv.py`
    (added 2026-09-08, replacing this rule's own "the gates do NOT detect this
    yet -- until they do it is a hand-check"). It is a SCREEN, NOT A PROOF --
    "is this integer" and "is this hot" are both inferred, so a hit is a
    candidate to confirm against a profile and a clean run means "nothing
    obvious", never "nothing present". It prints what it FILTERED for exactly
    that reason. `--all` shows every site.
  - **Its first run found 7 live instances of the `((x % n) + n) % n` idiom
    this rule names as the worst case**, none of them the one already fixed:
    `enemies.cpp` 372/444/469/547/679 (per-enemy, wrapping a lane index),
    `collision.cpp` 358, and `grid_geometry.cpp` 403 -- the last in the `grid`
    stage that is ~10 ms of the OG frame and is the largest unexamined item in
    the profile. NOT YET FIXED: `enemies.cpp` is mechanics-gated, so each needs
    its one-period bound shown before the exact-identity rewrite is safe.

---

## 5. Exemptions

Inline only, this grammar, no paraphrases:

```
/* @vfp-exempt R<id>[,R<id>...] — <reason>. measured <cost-or-n/a>. Verified <YYYY-MM-DD>. */
```

An exemption is a decision that integerizing the site **changes pixels or
hit tests**. It is not a TODO. Do not strip pins to "clean up" a file. Do
not add pins to cold code. Do not rewrite an exempted gameplay predicate
without a new measured OG-profile capture that says the rewrite is a win
*and* a fidelity verdict that says the hit set is unchanged.

Standing exemptions (corpus, not source comments) are in
`tools/runner/contracts/vfp_hotpath.md` §4: LUT range reduction, LUT
construction, mechanics-gated DOUBLE, procedural texture DSL, FFT twiddles.

---

## 6. What this pass already did — do not reopen

Closed on `perf/vfp-hotpath-remediation` (OG-profile hardware, 2026-09-06).
Busy scenes still drop — the OG CPU is the wall — but pacing is production.

| Work | Where | Do not undo |
|---|---|---|
| R1 FZ+DN | main + every 3DS worker | helpers, comments, call-before-float |
| Remaining presentation trig → LUT | `camera.cpp` planar lift, `touch_panel.cpp` hue wave, `arcade_flipper.cpp` lane angle | do not restore `std::sin` / `std::atan2` |
| R4 float literals / perf accumulators | `renderer_c3d.cpp` `buildView`, `perfTicksToMs` | no `double` tick math |
| Integer salt / shortest-arc | `rail_geometry.cpp` | no `(uint32_t)j` recasts, no float wrap `while (d > 128.0f)` |
| Hoisted invariants | `entity_geometry.cpp` (`nVerts`/`timeI`/`GLOW_SCALE`), collision/weapons bools | no per-iteration recasts |
| `@vfp-exempt` pins on swept windows, viewport culls, envelopes | collision, weapons, enemies, line/shatter/warp/rail/starfield, `renderer_c3d.cpp` | pins stay; they are the hit/discard set |
| `audit_arm11_math.py` | trig-LUT coverage + fixed-point candidates | keep the allowlist in that file matched to `math_lut.h` |
| R11 runtime int div/mod (2026-09-07) | `grid_geometry.cpp` lighten/transform, `line_geometry.cpp` ballLines, `entity_geometry.cpp` arcade border | wraps are compares now; do not "tidy" them back to `%` |
| Dead `vertex_tex1/2` removed (2026-09-07) | `engine.h` / `engine.cpp` `_resize_grid` | nothing ever read them; do not re-add a UV cache on the engine |

**Two facts about VFP11 that keep getting stated wrong** (both cost review time
on 2026-09-07): doubles are **hardware**, not emulated — `-mfpu=vfp` emits
`vmul.f64`/`vdiv.f64`/`vcvt.f64.f32` inline with no libcall; what they cost is
two registers each and a slower non-pipelined divide. And the gate cannot see a
promotion that arrives through a third-party macro — libctru's
`C3D_AngleFromDegrees` is `((_angle)*M_TAU/360.0f)` with `M_TAU` an unsuffixed
double, so it is an R4 MUST violation by the letter that no tool here reports.

New 3DS-compiled trig (`t2k_core/src/**` + `t2k_3ds/src/**`) MUST go through
the LUT or land on the documented allowlist in `audit_arm11_math.py`. The PC
tree is INFO only: one-path LUT on desktop is policy, not a 3DS violation.

---

## 7. Explicit non-rules

Do **not** report:

- An integer loop counter driving float arithmetic in the body.
- Mixed int and float in the same *function* when the hot loop stays in one
  domain.
- Float math in COLD PATH of any shape -- EXCEPT the two §3b rules, which are
  defect arguments rather than cost arguments and do apply in cold code.
- Float comparisons outside a loop (T2 is loop CONDITIONS only).
- Anything the profile does not show as hot.

Do **not**:

- Introduce `#ifdef` renderer forks. Backend split is by file
  (`renderer_c3d.cpp` vs `renderer_vk.cpp`), not ifdef.
- Touch `t2k_core/src/game/math_lut.h` as part of a "quick perf pass".
- Measure New-clocked and call it OG.
- Treat a green `.3dsx` as a perf pass. OG profile, looked-at frame.

---

## 8. Scope

Include: `t2k_core/src/**`, `t2k_3ds/src/**`, `t2k_3ds/Makefile`.
Exclude: `t2k_pc/**`, `t2k_core/third_party/**`, `t2k_core/tools/**`,
`t2k_3ds/tools/**`.

---

## 9. CIA home-menu identity (build-blocking)

Every new CIA build MUST ship the home-menu publisher **`TSEngine Devs`**, never
`TSEngine`. `TSEngine` is the engine's internal/source name (README, LICENSE,
NOTICE); it is not what the console shows. The three SMDH strings are authored
in the `t2k_3ds/Makefile` identity block:

| Field | Makefile var | Required value |
|---|---|---|
| short | `APP_TITLE` | `T2K` |
| long (shown on hover) | `APP_DESCRIPTION` | `Psychedelic Tube Shooter` |
| publisher | `APP_AUTHOR` | `TSEngine Devs` |

This is enforced, not assumed. `tools/build_production.sh` reads each field from
the Makefile and `tools/check_cia_smdh.py` re-reads the SMDH out of the BUILT
`.cia` and fails if any populated language slot disagrees. So a CIA whose
publisher says `TSEngine` (or anything other than the table above) cannot pass a
production build. Do not change `APP_AUTHOR` back to `TSEngine`, and do not
"align" the publisher to the engine name — the rename to `TSEngine Devs` is the
decision (user, 2026-09-27). Changing `APP_UNIQUE_ID` is a separate, larger
decision (it forks the install into a second title) and is out of scope here.

---

## 10. C++ implementation constraints — "C with classes"

Scope is §8. This is the **language** half of DOCTRINE.md "Performance doctrine
(C with classes)". The **numeric** half is §§3–4 and is deliberately not restated
here — two copies of R2/R4/R6/R11 is two copies to drift.

Do not introduce:

- **Virtual functions, virtual destructors, or inheritance that depends on
  virtual dispatch.** The reason is architecture, not the cycle argument §§1–4
  make: the renderer seam is chosen at **build time** by which tree is compiled
  (`renderer_c3d.cpp` vs `renderer_vk.cpp`, DOCTRINE.md "Rendering backend
  seam"), so a vtable would put a runtime decision where the build already made
  it. 0 virtuals in the tree today; `enemies.cpp:70` and the `enemies/arcade_*.cpp`
  headers say so at each dispatch site.
- **RTTI** — `dynamic_cast`, `typeid`.
- **Exceptions.** Not "exceptions for control flow": exceptions at all. Both
  build systems compile `-fno-exceptions -fno-rtti` (`t2k_3ds/Makefile`
  `CXXFLAGS`, `t2k_pc/CMakeLists.txt`), so they do not compile. Setup errors
  use return codes / status structs, as `vfp_set_fz_dn()` does.
- **Project-defined templates.** Concrete types and ordinary functions. This is
  **house style, not an ARM11 performance rule** — say it that way, because the
  opposite claim is false: a template is compile-time, and an inlined template
  over a fixed trip count is usually *cheaper* than the hand-duplicated concrete
  functions or the `void*`-punned seam it would replace. The only ARM11
  argument here is §1 item 4, code footprint in a 16 KB L1I with L2 off, and
  that argues for restraint, not for a ban. The real reasons are readability and
  one-implementation-per-behaviour. Do not defend this rule by claiming
  templates are slow.
- **Type-erased callables** (`std::function`, `std::bind`). 0 in the tree. The
  seam is free functions over `GameEngine&` and a flat C-API header
  (`rendering/render.h`), not stored callables. This one IS an ARM11 rule: a
  heap-allocated target, an indirect call, and no inlining.

In HOT PATH (§2) also:

- **No per-frame container growth where the size is known.** Preallocate and
  reuse — `grid_geometry.cpp textureLevel` resizes only when `n` changes, not
  every call. Where a container genuinely grows during play, that is shipped
  design and the rule is about its CONSEQUENCE, not the growth:
  `enemies.cpp:967` re-fetches the enemy after `push_back` because the
  reallocation invalidated the reference. Do not "optimise" the spawn vectors
  into fixed arrays without checking that invariant first.

On the 3DS worker threads, **respect the stack you were given**: SFX runs on
**8 KB** (`t2k_3ds/src/audio/sfx_3ds.cpp:359`) and music on **16 KB**
(`t2k_3ds/src/audio/music_3ds.cpp:275`). No recursion in those threads, and
no large local arrays or local containers — an overflow there is silent memory
corruption in the audio path, not a clean fault. Size locals against those
numbers before adding them.

Ordinary classes, structs, constructors, destructors, RAII, namespaces,
non-virtual member functions and local lambdas remain permitted. Use them to
make ownership and behaviour clear.

**Do not "tidy" `-fsingle-precision-constant` into `COMMONFLAGS`.** It is set
per-object for exactly three files (`t2k_3ds/Makefile:459-461`: `grid_geometry`,
`line_geometry`, `entity_geometry`). Widening it retypes every unsuffixed float
literal in the tree, which silently changes the **mechanics-gated DOUBLE** math
R4 protects — `engine.cpp:370-386` computes spawn counts in double, and a ULP
there moves a `round()` boundary and changes how many enemies a level spawns.
That is a fidelity regression dressed as a cleanup. The presentation files
already write `(float)M_PI` / `static_cast<float>(M_PI)` by hand; keep doing
that.

**Enforcement.** `python3 tools/runner/audit_c_with_classes.py` (wired into
`tools/check.sh`) now gates the zero-tolerance constructs of this section:
`virtual`, `override`, `dynamic_cast`, `typeid`, `throw`/`try`/`catch`,
`template`, `std::function`/`std::bind`, and the `<typeinfo>`/`<exception>`/
`<functional>` includes. The tree is at 0 of each, so the gate is green with no
baseline file — a hit is new code, and the fix is to not write it, not to bless
it. It strips comments and string literals before matching, because this tree
documents its own rules in prose ("no virtual, no vtable" at `enemies.cpp:70`)
and a naive grep flags that prose as code.

What the gate still cannot see: the per-frame heap-growth rule (it is violated
by correct shipped design, so a gate would fail on right code) and the worker
stack rule. Those stay review-only. DOCTRINE.md's claim that this section is
"enforced by the `perf-guard` agent" is stale — no such agent exists in
`.kilo/agents/` (which holds `swarm-worker`, `swarm-orchestrator`,
`swarm-validator`, `adversarial-review`).
