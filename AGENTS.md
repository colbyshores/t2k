# T2K — performance and platform contract

Revision: 2026-10-14, amended 2026-10-19 (perf gates frozen at v1.02 -- see §2
and §12).

Design language, fidelity ladder, and the rest of the doctrine live in
`docs/DOCTRINE.md`. This file records T2K's ARM11 performance rules, gameplay
safeguards, C++ house style, and 3DS platform requirements. The original math
remediation was closed by the 2026-09-06 OG-profile playtest. Preserve those
decisions; do not restore libm, double arithmetic, or float control flow to
remediated hot loops without the measurement and fidelity review below.

**Project policy and hardware facts are different.** R2/R3 and the language
bans are deliberately conservative rules for this engine. They are not proof
that every prohibited construct is always slow on every 3DS workload. When
adapting this contract, establish the new project's profile and invariants.

The original 3DS is the performance target. T2K's New 3DS OG profile
(`og_profile: true` in `sdmc:/3ds/t2k/t2k_config.json`) uses the 268 MHz CPU
setting with L2 disabled as a development proxy. Record the actual console,
clock, cache settings, and stereoscopic mode for every capture. An OG-profile
run on New 3DS is not a claim that every hardware characteristic is identical
to original 3DS. A New-clocked run is not an OG pass.

This revision corrects the timing and build-flag explanations and adds §11.
It does not certify a new build, rerun the historical measurements, or add
checks to the existing scripts. Historical paths, counts, and pass results
below describe the supplied project record; confirm them against the current
checkout before changing code. Sources are in §12.

---

## 1. What the ARM11 actually taxes

Floating-point arithmetic is supported in hardware. Costs depend on the
operation, dependencies, register pressure, and memory behavior:

1. **Transfers, conversions, and dependencies.** These are distinct operations,
   not a universal 20+ cycle penalty. ARM11 MPCore TRM §19.11, Table 19-16,
   lists the following timings [1]:

   | Operation | Manual mnemonic | Throughput interval | Latency |
   |---|---|---:|---:|
   | ARM → VFP single-register transfer | `FMSR` | 1 cycle | 4 cycles |
   | VFP → ARM single-register transfer | `FMRS` | 1 cycle | 2 cycles |
   | Integer ↔ float conversion | `FSITO` / `FTOSI` and variants | 1 cycle | 8 cycles |
   | Float comparison | `FCMP` and variants | 1 cycle | 4 cycles |
   | Copy comparison flags to ARM | `FMSTAT` | 1 cycle | 2 cycles |

   Throughput measures when another instruction can start; latency concerns
   result availability. Dependencies, forwarding, and serialization affect a
   sequence's timing. Do not add table entries and call the sum a measured
   loop cost. Inspect generated instructions and profile the actual loop.
2. **Subnormal handling.** Floating-point decay can reach subnormal values.
   VFP11 can require support-code handling in full-compliance mode; FZ changes
   underflow/subnormal behavior, and DN selects default NaNs [2]. These are
   deliberate numerical choices, not transparent optimizations. See R1 for
   libctru initialization and T2K's defensive checks.
3. **Register pressure.** There are 32 single-precision registers, overlapping
   16 double-precision registers. Doubles and long live ranges can increase
   spills; count generated loads/stores rather than source variables [1][2].
4. **Code footprint.** Excess inlining or duplicated instantiations can increase
   instruction-cache pressure. This is an argument for measuring code size
   and locality, not a claim that templates themselves are slow. §10 retains
   T2K's separate house-style restriction.

The old `FMSR`/`FMRS` mnemonics correspond to ARM↔VFP forms of `vmov`.
`vcvt` converts within the VFP register file; a transfer to an ARM register
is separate. The comparison path uses `vcmp` then a flags transfer through
`vmrs` (`FMSTAT` in the manual). Those instructions are not interchangeable
examples of a single boundary-crossing cost [1][2].

This contract does **not** prohibit floating-point math or mixing integer
and float values in the same function. Code footprint is review-only here.

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

The hot-path restrictions are R2–R6 and R7–R9/R11, with the documented
exemptions. R1 applies to thread initialization and FPSCR state; R10 applies
to the build. T1/T2, §9, §10's general language rules, and §11's correctness
requirements have their own wider scope.

Init, level load, menu logic, texture generation, parsing, and teardown are
normally COLD. Do not "clean up" them to satisfy hot-only math restrictions.
If work moves into a per-frame path, reassess it against the criteria above.
The annotation and frequency criteria also apply when a function is outside
the profile's top 15.

### PROJECT FROZEN — the staleness clocks are waived, deliberately (2026-10-19)

T2K shipped v1.02 and no further OG-profile perf campaign is planned. Two of
this file's clocks exist to **demand a re-measurement**: the corpus age limit
(`CORPUS_MAX_AGE_DAYS = 45`) and the exemption-pin age limit
(`PIN_MAX_AGE_DAYS = 180`). On a frozen tree that demand cannot be satisfied --
nobody is taking another capture -- so past their dates they stop being a
prompt and become permanent red. A permanently red gate is worse than a stale
corpus, because it trains the reader to skip the gate, and §2's whole argument
depends on the reader believing the gate.

So both clocks are waived from **2026-10-19** by `PROJECT_FROZEN_AT` in
`tools/runner/gates_vfp.py`. This is not a bug and must not be "tidied" back.

**What is NOT waived:** every substantive rule still runs -- R1–R11, T1/T2,
exemption shape, the `measured`/`Verified` pin grammar, register pressure.
Editing hot-path code still fails on its merits. Only the *age* checks are
silent. `PASS` after the freeze therefore means "clean under the freeze", not
"freshly measured", and the gate says so on its own output:

```
note: staleness clocks waived (corpus age, pin age): project frozen at 2026-10-19.
```

**To resume perf work**, set `VFP_FROZEN_AT=""` and take a fresh capture. Do not
edit `captured` in the corpus to a newer date: the gate rejects a date newer
than the newest `docs/validation/perf-*.log` precisely so the field cannot be
bumped without a real capture.

**A new target is not covered by this waiver.** The corpus is a 3DS OG-profile
capture; a hot-path list derived from a ~30 fps 3DS frame says nothing about a
90 Hz-per-eye VR budget, so it is not merely stale for a VR build, it is
inapplicable. `t2k_pc/**` is already outside §8's scope, which means a VR
renderer there is explicitly *ungated*, not compliant. New VR work needs its
own capture and its own corpus -- do not resume the 3DS clock and read a green
result as VR evidence.

---

## 3. MUST rules (build-blocking)

### R1 — Verify FPSCR FZ + DN on every float thread

Flush-to-Zero (bit 24) and Default-NaN (bit 25) MUST be enabled for T2K's
floating-point threads before application float work. This initialization
requirement applies regardless of hot/cold classification.

Current upstream libctru's `initThreadVars()` sets both bits, and its main
startup calls that initializer [5]. Verify the version and thread-entry path
actually linked by this project; do not assume all historical releases or
custom thread entry points have the same initialization. Main-thread state
alone is not evidence of a worker's state.

Keep T2K's explicit helpers/read-back as defensive enforcement. Do not claim
they necessarily provide a new speedup over an already initialized libctru
thread. FZ/DN change numerical semantics: do not toggle them within gameplay
or assume they preserve subnormal results or NaN payloads.

Shipped helpers (do not delete, do not `#ifdef` out, do not skip the
read-back on main):

| Thread | Helper | Call site |
|---|---|---|
| main | `vfp_set_fz_dn()` in `t2k_3ds/src/platform_3ds/main_3ds.cpp` | start of `main`, before `gfxInitDefault`; failure `return 1` immediately, never `goto quit` |
| music worker | `vfp_thread_fz_dn()` in `t2k_3ds/src/audio/music_3ds.cpp` | first line of `musicThread` |
| SFX worker | `vfp_thread_fz_dn()` in `t2k_3ds/src/audio/sfx_3ds.cpp` | SFX thread entry |
| renderer worker | `vfp_thread_fz_dn()` in `t2k_3ds/src/rendering/renderer_c3d.cpp` | renderer thread entry |
| shared audio workers | `vfp_thread_fz_dn()` in `t2k_core/src/audio/audio_thread.h` | 3DS branch only |

Every helper and call site MUST keep a rationale comment: decay can reach
subnormals; FZ/DN are the chosen numerical mode; each thread's state must be
established; modern libctru already initializes it on supported entry paths.
Unexplained FPSCR asm is a MUST finding. Updating the explanation must not
remove the helper, main read-back, or existing startup failure behavior.

A new 3DS worker thread that touches floats MUST get the same helper at
entry. Copy the existing comment; do not invent a second protocol.

### R2 — No float→int conversion in a hot loop body

Do not introduce float→int casts, float→int `vcvt` sequences, `lroundf`,
or array-index-from-float inside a hot loop without an exemption. Hoist an
invariant conversion or keep a naturally integer domain integer. An ordinary
integer cast is not a violation; not every `vcvt` is a float→int conversion.

This is T2K's conservative regression rule, not a fixed cycle estimate.
Inspect the generated code and preserve rounding, range, and gameplay
semantics. Never integerize a continuous quantity solely to satisfy a scan.

### R3 — No float comparison driving control flow in a hot loop

A `vcmp` followed by `vmrs` makes ARM control flow depend on VFP comparison
flags; it is not a float→integer→float conversion round trip. T2K restricts
new occurrences in hot loops pending profiling and a fidelity review.
It is not a universal claim that every float comparison needs replacing.

Gameplay swept windows (shot/enemy/player z-tests, spike lethality) are the
hit set. Integerizing them can change the game. Preserve their
`@vfp-exempt R3` pins; do not "fix" them. Here, R3 concerns loop control
and decisions inside hot loops; T2 separately addresses loop termination.

### R4 — No double precision in hot paths

Unexempted `double` arithmetic and promotions that force double operations
are forbidden in hot paths. Use `float` and `f`-suffixed literals; prefer
`(float)M_PI * 0.00017f` to `(float)(M_PI * 0.00017)`.
`std::sin(float)` has a float overload, so the function name alone is not
proof of double arithmetic [11]. R6 independently restricts hot-path libm
transcendentals. Perf accumulators in `renderer_c3d.cpp` are `float` on purpose.

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
dedicated float-result floor instruction. The recorded project build lowered
this path to `bl floorf` plus a spill frame, defeating the LUT optimization.
Lowering is compiler/input-dependent; inspect a new toolchain's output
rather than treating that sequence as universal. The leaf `(int)` truncation
and sign fixup in `fastSin` is load-bearing. Do not "clean" it.

Call `mathLutInit()` from every new entry point (including host harnesses)
before any `fast*`. Uninitialized tables are zero; every trig call returns 0.

### R10 — Build flags follow the devkitPro baseline and are verified

Use the supported devkitARM toolchain and the devkitPro application template
as the baseline. Its current architecture flags are [3]:

```makefile
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
```

These settings MUST reach the relevant compile and link commands. Equivalent
Makefile organization is allowed; checking a literal `ARCH` line alone is not
verification. Preserve the template's other applicable compilation and
packaging settings. This block is not a replacement for the whole Makefile.

- Keep `-mtp=soft`: it selects runtime-helper access to the thread-local
  storage pointer (`__aeabi_read_tp`). This is separate from the floating-point
  ABI; do not remove it as a supposed float optimization [4].
- Explicit `-mfpu=vfp` is permitted; GCC defines it as VFPv2. Verify effective
  settings with the installed devkitARM compiler rather than assuming generic
  ARM compiler defaults [4].
- `-mtpcs-leaf-frame` is **not required by this contract**. It concerns Thumb
  Procedure Call Standard leaf frames, not enabling the VFP. If retained,
  document the actual debugging/stack-frame requirement and verify its effect
  for the selected instruction mode [4]. Do not attribute speedups to it
  without evidence.
- Keep T2K's `-fno-math-errno` policy where applied. It allows optimization of
  functions such as `sqrtf` without preserving `errno` reporting. It is not
  what enables hardware floating-point `/` [6]. Code must not depend on math
  functions setting `errno` under this policy.
- Do not add `-ffast-math` or `-Ofast`, which enables it. Preserve the
  fidelity-sensitive arithmetic and the narrow per-object
  `-fsingle-precision-constant` scope in §10. Manual reciprocal hoists still
  need rounding/fidelity review [6].

For build-setting changes, record the toolchain/library versions, inspect a
clean build's commands and representative disassembly, and run the affected
hardware checks. Updating this text does not change the Makefile or its gates;
reconcile them in the repository. Do not loosen a check merely to obtain green.

---

## 3b. TEMPERATURE-INDEPENDENT rules (apply in COLD paths too)

Unlike the hot-only numeric restrictions, T1/T2 also apply in cold code,
subject to the bounded-normalization exemptions below. They address needless
iterative work and termination hazards. R1, R10, and the other explicitly
wider-scope requirements remain in force; T1/T2 are not the only rules in this
file that apply outside hot paths.

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
  Added 2026-09-07. **This ARM11 target has no integer divide instruction.**
  Runtime integer division/remainder generally lowers to software helpers
  such as `__aeabi_idiv` / `__aeabi_idivmod` (or unsigned equivalents).
  Calls can add register-save/spill costs. VFPv2 has hardware float division,
  but that is not permission to replace exact integer math with float math.
  Actual costs depend on operands and generated code; verify them.
  A **compile-time-constant** divisor is not automatically a finding:
  compilers can use multiplication, shifts, masks, or other simplifications.
  - `((x % n) + n) % n` can require **two** remainder operations/helper calls;
    confirm the emitted code. For positive `n`, where the value is proven
    to be within `-n <= v < 2*n` and the arithmetic cannot overflow,
    `if (v < 0) v += n; else if (v >= n) v -= n;` gives the exact wrapped
    integer result. Prove the bound and representation safety at each site.
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
    flagged above**, none of them the one already fixed:
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

An exemption records a reviewed reason to preserve a site, including changes
to pixels, hit tests, rounding, asset output, or a measured tradeoff. It is
not a TODO. Do not strip pins to "clean up" a file. Do not add hot-only R-rule
pins to cold code; the existing T1/T2 exemptions can apply there. Preserve
the established `@vfp-exempt T1` / `@vfp-exempt T2` forms and check parser
support before changing annotation syntax.

Do not rewrite an exempted gameplay predicate without a new measured
OG-profile capture that says the rewrite is a win *and* a fidelity verdict
that says the hit set is unchanged.

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

**Hardware support and macro blind spots:** VFPv2 supports basic double
arithmetic in hardware. With the appropriate target/ABI settings, operations
can use `vmul.f64`/`vdiv.f64`/`vcvt.f64.f32` without software emulation.
Register pressure and operation-specific timings still matter [1][2].

The original audit recorded an implicit promotion through Citro3D's
`C3D_AngleFromDegrees` macro and its `M_TAU` constant that the text gate did
not detect. Recheck the installed header, argument types, and generated code:
macro expansion can hide double arithmetic, while constant folding can remove
an operation entirely. Do not declare the current binary clear from a text scan.

New 3DS-compiled trig (`t2k_core/src/**` + `t2k_3ds/src/**`) MUST go through
the LUT or land on the documented allowlist in `audit_arm11_math.py`. The PC
tree is INFO only: one-path LUT on desktop is policy, not a 3DS violation.

---

## 7. Explicit non-rules

Do **not** report:

- An integer loop counter driving float arithmetic in the body.
- Mixed int and float in the same *function* when the hot loop stays in one
  domain.
- Ordinary float arithmetic in COLD PATH as a hot-path performance finding.
  R1's initialization requirement and §3b's rules still apply.
- Float comparisons outside a loop as R3/T2 findings. T2 concerns loop
  conditions; R3 also covers decisions inside hot loops.
- Hot-only math findings in code that meets none of §2's three criteria.
  A frequency threshold or explicit annotation can establish scope even when
  a function is outside the captured top 15.

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
  opposite claim is false: templates are instantiated at compile time and
  can produce code as efficient as ordinary concrete functions. Compare actual
  generated code instead of assuming either form is cheaper. The relevant ARM11
  argument here is §1 item 4, code footprint in a 16 KB L1I with L2 off, and
  that argues for restraint, not for a ban. The real reasons are readability and
  one-implementation-per-behaviour. Do not defend this rule by claiming
  templates are slow.
- **`std::function` and `std::bind`.** The project record has 0 in the tree.
  Keep free functions over `GameEngine&` and the flat C-API header
  (`rendering/render.h`). `std::function` adds type erasure and can introduce
  allocation/indirect-call costs; small targets need not allocate.
  `std::bind` is a binding wrapper, not inherently type erasure, and neither
  construct universally prevents inlining [11]. The ban remains T2K policy;
  do not defend it as a universal hardware fact.

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

---

## 11. 3DS platform requirements

These rules cover the platform concerns needed alongside the math contract.
They apply to the 3DS backend and its shared interfaces; they do not require
Citro2D, RomFS, new workers, or a new rendering architecture where the project
does not use them. Correctness requirements apply in cold paths too.

**Enforcement status:** review and hardware validation. The existing math and
language scripts are not claimed to check P1–P6. For changes to these paths,
demonstrate compliance or record the unresolved requirement. Do not mark a
hardware check passed because compilation or a text scan succeeded.

### P1 — Application lifecycle, sleep, and resume

- The application MUST service `aptMainLoop()` regularly through its existing
  main-loop integration and exit when it returns false. libctru uses it to
  process sleep/HOME behavior and closure [7]. Do not replace it with an
  unconditional loop or block it indefinitely on a worker.
- Use `aptHook` when application-specific suspend/resume or sleep/wake work
  is needed. Keep hook storage valid until unregistered. Coordinate the
  renderer, audio, streaming, and shared state around those transitions;
  rely on libctru's service handling rather than duplicating it blindly [7].
- T2K MUST not advance gameplay by the wall-clock interval spent suspended.
  Rebase the frame timer on resume under the existing pause policy. Avoid
  stale queued input and verify that audio resumes without duplicate workers.
- Initialization failures and normal shutdown MUST release only resources
  successfully acquired, in dependency order. Stop producers and settle
  outstanding work before destroying their resources. Keep R1's immediate
  startup failure path; do not jump into uninitialized cleanup.

### P2 — Citro3D/Citro2D frame and state ownership

- Initialize the graphics libraries used by the selected backend before
  creating or using their resources; check fallible calls. Follow the official
  examples for library order, render targets, and teardown [10].
- Give rendering state a documented owner. Workers may prepare data, but
  MUST not concurrently mutate the shared Citro3D/Citro2D state without a
  verified serialization design. Preserve T2K's existing renderer-thread
  ownership rather than moving GPU calls between threads casually.
- Draw only after a successful `C3D_FrameBegin`, and pair each successful
  begin with one `C3D_FrameEnd`. A failed/nonblocking begin does not authorize
  drawing. Select the intended target before issuing its commands [8].
- If mixing Citro2D with custom Citro3D rendering, flush pending 2D batches
  with `C2D_Flush()` before changing their required state, establish the
  custom pass's state, and call `C2D_Prepare()` when returning to 2D rendering.
  When Citro2D is the sole GPU user, its documentation permits one preparation
  at startup. A 2D batch flush is not proof that the GPU has finished [9].
- Preserve both-eye targets, projection, and slider behavior. Update gameplay
  once per simulation step and render the appropriate eye views; do not
  advance gameplay once per eye. Reuse eye-independent preparation where
  profiling and fidelity checks support it.

### P3 — GPU completion, cache visibility, and frame pacing

- Keep vertex/index buffers, textures, and other submitted resources valid
  until the GPU is finished with them. Do not overwrite, resize, or free an
  in-flight resource. Put the completion boundary **before** CPU reuse;
  waiting later in the frame cannot repair an earlier overwrite.
- Distinguish three obligations: CPU-thread ownership, CPU/GPU cache
  visibility, and GPU completion. A lock or cache flush alone is not a GPU
  completion fence. Use the appropriate supported allocation, alignment,
  transfer, and synchronization APIs for each resource.
- Current Citro3D waits for its previous queue in `C3D_FrameBegin()` and,
  with `C3D_FRAME_SYNCDRAW`, also performs frame synchronization. Its default
  `C3D_FrameEnd(0)` path flushes linear memory before queue execution [8].
  Preserve that behavior unless an alternative is measured and verified.
  Changing end flags to `GX_CMDLIST_FLUSH` changes the flush policy; it does
  not mean every CPU-written data buffer is automatically covered.
- Do not add redundant VBlank waits, manual swaps, per-draw waits, or cache
  flushes around an already managed frame path without identifying the need.
  Measure CPU preparation, GPU work, and waiting separately. A wait time is
  not automatically time spent doing CPU math.

### P4 — Worker synchronization and shutdown

- Document ownership and lifetime for data shared by simulation, rendering,
  audio, and I/O. Use supported locks/events/atomics and an explicit handoff.
  `volatile` alone is not synchronization. A buffer must not be read while
  another thread rewrites or reallocates it.
- Keep queues and buffering within a stated memory budget. Do not busy-spin
  for routine work or hold a shared lock across slow I/O or a GPU wait when
  the producer/consumer needs that lock to make progress.
- On shutdown, signal workers, unblock their waits, and confirm completion
  before freeing shared data. For owned, joinable libctru threads, join before
  freeing the thread object. Detached workers require a separately verified
  lifetime design; do not assume they are finished because exit was requested.
- Apply R1 to new floating-point worker entry paths and respect §10's actual
  stack budgets. Do not increase thread count, priority, affinity, or stack
  allocation as an unmeasured performance fix.

### P5 — RomFS, SD access, and streaming

- If using RomFS, ensure it is packaged for each shipped format, mount it
  successfully with `romfsInit()` (or the existing owning wrapper) before
  access, and release it after its users finish. RomFS is read-only [10].
  Keep writable saves/configuration on the project's intended writable path.
- Use explicit paths such as `romfs:/...` and T2K's existing `sdmc:/3ds/t2k/...`
  conventions. Do not assume a working directory or silently move user files.
- Check open/read/write/close results as applicable. Distinguish a complete
  asset from a short read; distinguish a successful save from a failed write
  or close. Missing assets, failed mounts, and unavailable/full storage need
  a defined recovery or error path, not an unbounded retry in the frame loop.
- Preload small reusable assets where appropriate. For larger data or music,
  use measured buffering/streaming with a bounded memory budget. Keep
  unpredictable blocking reads/writes, asset decoding, directory scans, and
  verbose logging out of frame-critical work unless a measured exception is
  documented. A `romfs:` path is not evidence that access is free or resident.
- Loading and streaming changes MUST preserve asset bytes and the current
  audio/gameplay behavior. Do not introduce a new worker or load everything
  into RAM merely to satisfy this rule; first inspect the existing path.

### P6 — Evidence for changes to platform paths

Select checks relevant to the changed behavior and record results explicitly:

| Area changed | Required evidence |
|---|---|
| Build or math | Clean build commands, tool/library versions, relevant disassembly, affected fidelity checks, and a comparable hardware capture |
| Rendering or synchronization | Busy-scene pacing, both-eye/slider behavior, buffer reuse under load, and successful resource teardown |
| Lifecycle or workers | HOME return, lid sleep/wake, repeated pause/resume, clean exit, and no stuck waits or duplicate workers |
| Filesystem or streaming | Packaged assets in each shipped format, missing/read-error handling, relevant save failures, and streaming behavior under load |

Record hardware, scene/workload, clock/cache settings, build revision, and
stereoscopic mode. Separate actual original-3DS testing from New 3DS OG-profile
testing. If actual original hardware is unavailable, state that gap; do not
claim it passed. A documentation-only revision is not a new performance pass.

---

## 12. References and revision record

Primary sources consulted for this revision, 2026-10-14. Upstream `master`
links and compiler documentation can change; implementation reviews must
record the installed versions/commits rather than infer them from these links.

1. [ARM11 MPCore Technical Reference Manual, DDI 0360F](https://documentation-service.arm.com/static/5e8e1e0388295d1e18d368b2),
   §19.11 / Table 19-16 (printed pp. 19-25–19-26) and dependency examples.
2. [VFP11 Technical Reference Manual, DDI 0274H](https://documentation-service.arm.com/static/5e8e21eb88295d1e18d375b0),
   register organization, numerical modes, and execution timing.
3. [devkitPro application Makefile](https://github.com/devkitPro/3ds-examples/blob/master/templates/application/Makefile).
4. [GCC ARM options](https://gcc.gnu.org/onlinedocs/gcc/ARM-Options.html),
   `-mtp`, `-mfpu`, and `-mtpcs-leaf-frame`.
5. [libctru thread-variable/startup initialization](https://github.com/devkitPro/libctru/blob/master/libctru/source/system/syscalls.c),
   `initThreadVars()` and `__system_initSyscalls()`.
6. [GCC optimization options](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html),
   `-fno-math-errno`, `-ffast-math`, and reciprocal transformations.
7. [libctru APT implementation](https://github.com/devkitPro/libctru/blob/master/libctru/source/services/apt.c),
   `aptMainLoop()`, sleep/HOME handling, and hooks.
8. [Citro3D render queue](https://github.com/devkitPro/citro3d/blob/master/source/renderqueue.c),
   frame synchronization, queue completion, and cache flushing.
9. [Citro2D base API](https://github.com/devkitPro/citro2d/blob/master/include/c2d/base.h),
   `C2D_Prepare()` and `C2D_Flush()`.
10. devkitPro examples:
    [application lifecycle](https://github.com/devkitPro/3ds-examples/blob/master/templates/application/source/main.c),
    [stereoscopic Citro2D](https://github.com/devkitPro/3ds-examples/blob/master/graphics/gpu/stereoscopic_2d/source/main.cpp),
    [RomFS](https://github.com/devkitPro/3ds-examples/blob/master/romfs/source/main.c).
11. C++ working draft:
    [`<cmath>` overloads](https://eel.is/c++draft/cmath.syn),
    [`std::function` construction](https://eel.is/c++draft/func.wrap.func.con),
    [`std::bind`](https://eel.is/c++draft/func.bind.bind).

### Changes in this revision

- Replaced the blanket VFP boundary-cost claim with instruction-specific
  timing and dependency guidance; corrected the R2/R3 explanation.
- Based R10 on devkitPro's flags, restored `-mtp=soft` to the required baseline,
  and separated optional flags from supported math policy.
- Acknowledged libctru's FZ/DN initialization while retaining T2K's helpers,
  read-back, and per-thread numerical requirements.
- Clarified hot/cold scope and cold-path exemptions without removing standing
  gameplay, LUT, asset, or mechanics-gated-double safeguards.
- Added review requirements for rendering, synchronization, lifecycle,
  workers, storage, and hardware validation. No new audit coverage is claimed.
- Corrected the explanation of `std::sin(float)`, `std::function`, and
  `std::bind` while preserving the existing project restrictions.
- Qualified compiler-lowering claims for integer division, LUT range reduction,
  and macro promotions; retained their existing optimization safeguards.

### Amendment, 2026-10-19 — perf gates frozen at v1.02

The game shipped. No further OG-profile capture is planned, so the two clocks
that demand one were waived rather than left to go permanently red:

- `PROJECT_FROZEN_AT` in `tools/runner/gates_vfp.py` silences `R-CORPUS` (corpus
  age) and `R-EXPIRED` (pin age) only. Every substantive rule still runs.
- The waiver prints a `note:` line on the gate's own output, so a green result
  cannot be read as a fresh measurement.
- `VFP_FROZEN_AT=""` restores both clocks for resumed perf work.
- The waiver is scoped to the 3DS campaign. It is not evidence for any other
  target; see §2's "A new target is not covered by this waiver."

No code outside `tools/runner/gates_vfp.py` changed, and no historical
measurement in this file was altered.
