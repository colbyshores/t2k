from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.runner.gates_vfp import (  # noqa: E402
    Finding,
    gate_r1,
    gate_r10,
    run_audit,
    scan_file,
    seed_map,
)

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "vfp"
EMPTY_SEED = seed_map({"seed": [], "cold_always": []})
HOT_SEED = seed_map(
    {
        "seed": [{"file": p.name, "symbols": ["*"], "why": "fixture"}]
        for p in FIXTURES.glob("*.cpp")
    }
)


def rules(path: Path, *, force_hot: bool = True, cold: list[str] | None = None) -> list[Finding]:
    return scan_file(path, HOT_SEED if force_hot else EMPTY_SEED, cold or [], force_hot=force_hot)


def rule_ids(findings: list[Finding], severity: str | None = None) -> set[str]:
    out = set()
    for item in findings:
        if severity and item.severity != severity:
            continue
        out.add(item.rule)
    return out


class MakefileAbiTests(unittest.TestCase):
    def test_good_makefile(self) -> None:
        findings = gate_r10(FIXTURES / "makefile_abi.mk")
        self.assertEqual(findings, [])

    def test_bad_makefile(self) -> None:
        findings = gate_r10(FIXTURES / "makefile_abi_bad.mk")
        ids = rule_ids(findings, "MUST")
        self.assertIn("R10", ids)
        blob = " ".join(f.detail for f in findings)
        self.assertTrue("soft" in blob or "mfpu" in blob or "errno" in blob)


class HotPathScanTests(unittest.TestCase):
    def test_integer_counter_float_body_ok(self) -> None:
        findings = rules(FIXTURES / "hot_ok.cpp")
        self.assertEqual(rule_ids(findings, "MUST"), set())

    def test_r2_cast_in_loop(self) -> None:
        findings = rules(FIXTURES / "hot_r2.cpp")
        names = {f.symbol for f in findings if f.rule == "R2"}
        self.assertIn("hot_r2", names)
        self.assertNotIn("hot_r2_hoisted", names)

    def test_r3_float_compare(self) -> None:
        findings = rules(FIXTURES / "hot_r3.cpp")
        names = {f.symbol for f in findings if f.rule == "R3"}
        self.assertIn("hot_r3", names)
        self.assertNotIn("hot_r3_int_pred", names)

    def test_r4_unsuffixed_and_double(self) -> None:
        findings = rules(FIXTURES / "hot_r4.cpp")
        names = {(f.symbol, f.rule) for f in findings if f.rule == "R4"}
        self.assertTrue(any(sym == "hot_r4_bad" for sym, _ in names))
        self.assertTrue(any(sym == "hot_r4_double" for sym, _ in names))
        self.assertFalse(any(sym == "hot_r4_ok" for sym, _ in names))

    def test_r5_float_induction_and_index(self) -> None:
        findings = rules(FIXTURES / "hot_r5.cpp")
        names = {f.symbol for f in findings if f.rule in {"R5", "R2"}}
        self.assertIn("hot_r5_for_float", names)
        self.assertIn("hot_r5_index", names)

    def test_r6_libm_vs_lut(self) -> None:
        findings = rules(FIXTURES / "hot_r6.cpp")
        names = {f.symbol for f in findings if f.rule == "R6"}
        self.assertIn("hot_r6_libm", names)
        self.assertNotIn("hot_r6_lut", names)

    def test_r7_inner_vs_outer(self) -> None:
        findings = rules(FIXTURES / "hot_r7.cpp")
        inner = [f for f in findings if f.rule == "R7" and f.symbol == "hot_r7_inner"]
        outer = [f for f in findings if f.rule == "R7" and f.symbol == "hot_r7_outer"]
        self.assertTrue(inner)
        self.assertTrue(all(f.severity == "SHOULD" for f in inner))
        self.assertTrue(outer)
        self.assertTrue(any("inner_loop=false" in f.detail for f in outer))

    def test_r8_fixed_point_mix(self) -> None:
        findings = rules(FIXTURES / "hot_r8.cpp")
        self.assertIn("R8", rule_ids(findings))

    def test_cold_double_zero_findings(self) -> None:
        findings = rules(FIXTURES / "cold_double.cpp", force_hot=False)
        self.assertEqual(findings, [])

    def test_inline_exempt_r3(self) -> None:
        findings = rules(FIXTURES / "exempt_r3.cpp")
        self.assertNotIn("R3", rule_ids(findings, "MUST"))

    def test_math_lut_standing_exemption(self) -> None:
        findings = rules(FIXTURES / "math_lut.h")
        must = [f for f in findings if f.severity == "MUST"]
        self.assertEqual(must, [], [asdictish(f) for f in must])


def asdictish(finding: Finding) -> str:
    return f"{finding.rule}:{finding.symbol}:{finding.detail}"


class RepoContractTests(unittest.TestCase):
    def test_r10_real_makefile_passes(self) -> None:
        findings = gate_r10(ROOT / "t2k_3ds" / "Makefile")
        self.assertEqual(findings, [])

    def test_r1_real_tree_fails(self) -> None:
        # Tasks 2+3 committed main + worker FPSCR fixes with VFPU comments;
        # the real tree is R1-clean.
        findings = gate_r1(ROOT)
        must = [f for f in findings if f.rule == "R1" and f.severity == "MUST"]
        self.assertEqual(must, [], [asdictish(f) for f in must])

    def test_r1_fpscr_comment_required(self) -> None:
        from tools.runner.gates_vfp import _fpscr_comment_ok

        bare = "static void h() { unsigned f; __asm__ volatile (\"vmrs %0, fpscr\" : \"=r\"(f)); }"
        self.assertFalse(_fpscr_comment_ok(bare))
        documented = (
            "// VFPU (R1): denormals stall without FZ; per-thread FPSCR FZ|DN.\n" + bare
        )
        self.assertTrue(_fpscr_comment_ok(documented))

    def test_run_audit_writes_fail_on_r1(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "vfp_audit.json"
            result = run_audit(ROOT, ingest_profile=False)
            # R1/R10 remediated; remaining MUSTs are R2-R6/R8 game-logic work.
            self.assertFalse(any(f.rule == "R1" for f in result.findings))
            self.assertFalse(any(f.rule == "R10" for f in result.findings))
            self.assertFalse(
                any(
                    f.file.endswith("math_lut.h") and f.symbol == "fastSin" and f.rule in {"R2", "R3", "R5"}
                    for f in result.findings
                )
            )


class R1WorkerEntryTests(unittest.TestCase):
    """R1 asks the WORKER, not the file it happens to live in.

    These are regression tests for the shape that made the old gate a presence
    check: a worker whose FPSCR helper call had been deleted still passed, because
    the helper's DEFINITION was in the same file and `fpscr_ok` matched the name
    wherever it appeared. Every case below is a deletion or an addition that the
    old gate scored as compliant.
    """

    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp
        self._repo = gates_vfp.REPO_ROOT
        self.tmp = Path(tempfile.mkdtemp(prefix="t2k-r1-"))
        for sub in ("t2k_core/src", "t2k_3ds/src"):
            shutil.copytree(ROOT / sub, self.tmp / sub)
        gates_vfp.REPO_ROOT = self.tmp

    def tearDown(self) -> None:
        self.g.REPO_ROOT = self._repo
        shutil.rmtree(self.tmp, ignore_errors=True)

    def r1(self) -> list[Finding]:
        return [f for f in self.g.gate_r1(self.tmp) if f.rule == "R1"]

    def cut(self, rel: str, needle: str) -> None:
        path = self.tmp / rel
        text = path.read_text()
        self.assertIn(needle, text, rel)
        path.write_text(text.replace(needle, "    // deleted", 1))

    def probe(self, body: str) -> None:
        (self.tmp / "t2k_3ds" / "src" / "probe_worker.cpp").write_text(body)

    def symbols(self, findings: list[Finding]) -> set[str]:
        return {f.symbol for f in findings}

    def test_clean_tree_is_r1_clean(self) -> None:
        self.assertEqual([asdictish(f) for f in self.r1()], [])

    def test_tex_prefetch_helper_call_removed_is_must(self) -> None:
        # The worker is created in c3d/04_setup.inc and defined in the SIBLING
        # fragment c3d/03_tev.inc, so only a unit-aware lookup can see this.
        self.cut(
            "t2k_3ds/src/rendering/c3d/03_tev.inc",
            "    vfp_thread_fz_dn();  // before any float work on this thread (R1)",
        )
        findings = self.r1()
        self.assertIn("threadCreate", self.symbols(findings))
        site = [f for f in findings if f.symbol == "threadCreate"]
        self.assertTrue(all(f.severity == "MUST" for f in site))
        self.assertEqual(site[0].file, "t2k_3ds/src/rendering/c3d/04_setup.inc")

    def test_music_helper_call_removed_is_must(self) -> None:
        # music_3ds.cpp DEFINES vfp_thread_fz_dn two lines above musicThread, so
        # the old file-level presence check could not fail this deletion.
        self.cut(
            "t2k_3ds/src/audio/music_3ds.cpp",
            "    vfp_thread_fz_dn();  // before any float work on this thread (R1)",
        )
        findings = self.r1()
        self.assertIn("threadCreate", self.symbols(findings))
        self.assertTrue(all(f.severity == "MUST" for f in findings))

    def test_analyzer_helper_call_removed_is_found(self) -> None:
        # Started through the shared sys::Thread wrapper, so the site is a
        # `.start(` call (SHOULD: the wrapper is std::thread off-3DS) and the
        # shared-worker row itself goes MUST once nothing in the unit runs it.
        self.cut("t2k_core/src/audio/audio_analysis.cpp", "    vfp_thread_fz_dn();")
        findings = self.r1()
        self.assertEqual(self.symbols(findings), {"thread-site", "threadCreate", "Thread::start"})
        self.assertTrue(any(f.symbol == "Thread::start" and f.severity == "SHOULD" for f in findings))

    def test_new_worker_without_helper_is_must(self) -> None:
        self.probe(
            "void probeWorker(void*) {\n"
            "    volatile float x = 1.0f;\n"
            "    (void)x;\n"
            "}\n"
            "void start_probe() {\n"
            "    s_thread = threadCreate(probeWorker, nullptr, 8 * 1024, 0, -2, false);\n"
            "}\n"
        )
        findings = [f for f in self.r1() if f.file.endswith("probe_worker.cpp")]
        self.assertEqual([f.symbol for f in findings], ["threadCreate"])
        self.assertEqual(findings[0].severity, "MUST")

    def test_new_worker_that_calls_the_helper_is_clean(self) -> None:
        self.probe(
            "void vfp_thread_fz_dn();\n"
            "void probeWorker(void*) {\n"
            "    vfp_thread_fz_dn();\n"
            "    volatile float x = 1.0f;\n"
            "    (void)x;\n"
            "}\n"
            "void start_probe() {\n"
            "    s_thread = threadCreate(probeWorker, nullptr, 8 * 1024, 0, -2, false);\n"
            "}\n"
        )
        self.assertEqual([asdictish(f) for f in self.r1()], [])

    def test_wrapper_started_worker_without_helper_is_should(self) -> None:
        self.probe(
            "namespace sys { struct Thread { bool start(void (*fn)(void*), void* a, int s, int d, int c); }; }\n"
            "sys::Thread s_probe;\n"
            "void probeMain(void*) { volatile float x = 1.0f; (void)x; }\n"
            "void go() { s_probe.start(probeMain, nullptr, 8192, 0, -2); }\n"
        )
        findings = [f for f in self.r1() if f.file.endswith("probe_worker.cpp")]
        self.assertEqual([f.symbol for f in findings], ["Thread::start"])
        self.assertEqual(findings[0].severity, "SHOULD")


class R1HelperTests(unittest.TestCase):
    """The three questions the old gate collapsed into one."""

    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp

    def test_definition_is_offered_not_established(self) -> None:
        defines = "static inline void vfp_thread_fz_dn() { }\n"
        self.assertTrue(self.g.fpscr_ok(defines))
        self.assertFalse(self.g.fpscr_established(defines))

    def test_call_establishes(self) -> None:
        self.assertTrue(self.g.fpscr_established("void w(void*) { vfp_thread_fz_dn(); }"))

    def test_inline_asm_establishes(self) -> None:
        text = "unsigned f; __asm__ volatile(\"vmrs %0, FPSCR\"); f |= (1u << 24) | (1u << 25);"
        self.assertTrue(self.g.fpscr_established(text))

    def test_worker_name_extraction(self) -> None:
        code = "h = threadCreate(texPrefetchWorker, r, 64 * 1024, prio, core, false);"
        self.assertEqual(self.g.worker_name_at(code, code.index("threadCreate"), "threadCreate"), "texPrefetchWorker")
        code2 = "s_thread.start(analyzerThreadMain, nullptr, 16 * 1024);"
        self.assertEqual(self.g.worker_name_at(code2, code2.index("s_thread"), "Thread::start"), "analyzerThreadMain")

    def test_verdict_is_none_for_an_unbound_pointer(self) -> None:
        # The shared wrapper's first argument is the parameter `fn`; there is no
        # routine to read, and that is reported as undecidable rather than as a
        # pass or a finding.
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "w.cpp"
            path.write_text("void go() { threadCreate(fn, arg, 8192, 0, -2, false); }\n")
            self.assertIsNone(self.g.worker_helper_verdict("fn", [path]))

    def test_include_closure_is_one_hop_not_the_whole_tree(self) -> None:
        # Transitive closure merges the repository: audio_thread.h reaches most of
        # t2k_core through engine.h, and a presence test over that is always true.
        files = self.g.swept_files(ROOT)
        closures = self.g.include_closures(files)
        members = closures[str((ROOT / "t2k_core/src/audio/audio_thread.h").resolve())]
        self.assertLess(len(members), len(files) // 4)
        self.assertNotIn((ROOT / "t2k_core/src/game/engine.cpp").resolve(), members)

    def test_include_closure_reaches_a_sibling_fragment(self) -> None:
        files = self.g.swept_files(ROOT)
        closures = self.g.include_closures(files)
        setup = closures[str((ROOT / "t2k_3ds/src/rendering/c3d/04_setup.inc").resolve())]
        self.assertIn((ROOT / "t2k_3ds/src/rendering/c3d/03_tev.inc").resolve(), setup)
        self.assertIn((ROOT / "t2k_3ds/src/rendering/renderer_c3d.cpp").resolve(), setup)


class IncFragmentHotnessTests(unittest.TestCase):
    """`.inc` fragments inherit the hotness of the .cpp that pastes them.

    The corpus names renderer_c3d.cpp with symbols ["*"], and the ten
    c3d/*.inc fragments ARE that translation unit -- every symbol the profile
    captured is defined in a fragment. Matching hotness by file path rated the
    renderer's whole body cold.
    """

    def setUp(self) -> None:
        from tools.runner import gates_vfp

        self.g = gates_vfp
        self.corpus = json.loads(
            (ROOT / "tools/runner/contracts/hotpath_corpus.json").read_text()
        )

    def files(self, root: Path) -> list[Path]:
        out: list[Path] = []
        for base in (root / "t2k_core" / "src", root / "t2k_3ds" / "src"):
            if base.exists():
                out.extend(p for p in base.rglob("*") if p.suffix in self.g.CXX_EXTS)
        return out

    def test_c3d_fragments_inherit_the_renderer_entry(self) -> None:
        seed = self.g.seed_map(self.corpus)
        entries = {e.rel: e for e in self.g.fragment_hot_entries(ROOT, self.files(ROOT), seed)}
        stars = entries.get("t2k_3ds/src/rendering/c3d/05_stars.inc")
        self.assertIsNotNone(stars)
        self.assertEqual(stars.symbols, ("*",))
        self.assertIn("renderer_c3d.cpp", stars.why)

    def test_fragment_without_a_hot_includer_stays_cold(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_dir = root / "t2k_core/src/util"
            cold_dir.mkdir(parents=True)
            (cold_dir / "table.inc").write_text("static const int T[] = {1, 2};\n")
            (cold_dir / "user.cpp").write_text('#include "table.inc"\nint f() { return T[0]; }\n')
            entries = self.g.fragment_hot_entries(root, self.files(root), self.g.seed_map(self.corpus))
            self.assertEqual(entries, [])

    def test_baseline_absorbs_known_and_blocks_new(self) -> None:
        f = Finding("R3", "MUST", "x/y.inc", 12, "fn", True, "seed", None, "float comparison")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "b.txt"
            path.write_text("")
            self.assertEqual(self.g.filter_inc_hot([f], path), ([f], 0))
            path.write_text(self.g.inc_hot_key(f) + "\n")
            self.assertEqual(self.g.filter_inc_hot([f], path), ([], 1))
            # A multiset: a second copy of a baselined key is one more than allowed.
            self.assertEqual(self.g.filter_inc_hot([f, f], path)[1], 1)

    def test_real_tree_fragments_are_fully_baselined(self) -> None:
        # The shipped baseline covers every site the fragment coverage currently
        # finds, so the gate is green on a clean tree -- and only because of that
        # coverage: an unlisted site is a blocker, not a shrug.
        findings, _ = self.g.scan_tree(ROOT, self.corpus, ingest_profile=False)
        self.assertEqual([f for f in findings if f.file.endswith(".inc")], [])



if __name__ == "__main__":
    unittest.main()
