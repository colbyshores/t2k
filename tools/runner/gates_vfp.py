from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from datetime import date
from pathlib import Path
from typing import Iterator, Literal, Mapping, Sequence

Severity = Literal["MUST", "SHOULD"]
Status = Literal["PASS", "FAIL"]

# An @vfp-exempt pin is a CLAIM that integerizing the site changes pixels or hit
# tests, backed by a `Verified <date>`. A claim with no expiry is a claim nobody
# rechecks: AGENTS.md names reflexive stamping as the failure mode of a large
# exemption corpus, and an undated-or-never-rechecked pin is how that happens.
# Past PIN_MAX_AGE_DAYS the pin is a MUST finding, and the remedy is the one the
# doctrine already requires -- a fresh measured capture, then bump the date.
PIN_MAX_AGE_DAYS = int(os.environ.get("VFP_PIN_MAX_AGE_DAYS", "180"))
# The hot-path corpus is "top 15 of a CAPTURED profile" (AGENTS.md 2). Against a
# stale capture that phrase means nothing, so the corpus must declare when it was
# captured and go stale on the same clock.
CORPUS_MAX_AGE_DAYS = int(os.environ.get("VFP_CORPUS_MAX_AGE_DAYS", "45"))


def _age_days(iso: str, today: date | None = None) -> int | None:
    try:
        y, m, d = (int(x) for x in iso.strip().split("-"))
        return (today or date.today()).toordinal() - date(y, m, d).toordinal()
    except (ValueError, AttributeError):
        return None


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER_DIR = Path(__file__).resolve().parent
CONTRACTS_DIR = RUNNER_DIR / "contracts"
DEFAULT_CORPUS = CONTRACTS_DIR / "hotpath_corpus.json"
DEFAULT_LOG = RUNNER_DIR / "logs" / "vfp_audit.json"
MAKEFILE_3DS = REPO_ROOT / "t2k_3ds" / "Makefile"
SRC_ROOTS = (
    REPO_ROOT / "t2k_core" / "src",
    REPO_ROOT / "t2k_3ds" / "src",
)
EXCLUDED_PREFIXES = (
    REPO_ROOT / "t2k_pc",
    REPO_ROOT / "t2k_core" / "third_party",
    REPO_ROOT / "t2k_core" / "tools",
    REPO_ROOT / "t2k_3ds" / "tools",
)

CXX_EXTS = {".c", ".h", ".cpp", ".hpp", ".cc", ".hh"}
INIT_NAME_RE = re.compile(
    r"(init|load|create|destroy|reset|parse|shutdown|seed)", re.IGNORECASE
)
THREAD_CREATE_RE = re.compile(r"\bthreadCreate\s*\(")
FPSCR_HELPER_RE = re.compile(
    r"\b(vfp_set_fz_dn|ts_vfp_init|vfp_init_fpscr|ts_vfp_set_fpscr)\b"
)
FPSCR_ASM_RE = re.compile(
    r"\b(vmsr|vmrs)\b.*\bFPSCR\b"
    r"|mcr\s+p10\s*,\s*7"
    r"|FPSCR",
    re.IGNORECASE,
)
EXEMPT_RE = re.compile(
    r"/\*\s*@vfp-exempt\s+(?P<rules>R\d+(?:\s*,\s*R\d+)*)\s+—\s+"
    r"(?P<reason>.+?)\.\s*"
    r"(?:measured\s+(?P<measured>\S+)\s*\.\s*)?"
    r"Verified\s+(?P<verified>\d{4}-\d{2}-\d{2})\s*\.\s*\*/",
    re.DOTALL,
)
HOTPATH_ANN_RE = re.compile(r"/\*\s*@hotpath\s*\*/")
FUNC_RE = re.compile(
    r"(?P<head>^[\w:<>,\s\*&]+?\b(?P<name>[A-Za-z_]\w*)\s*\([^;{]*\))\s*\{",
    re.MULTILINE,
)
FOR_FLOAT_RE = re.compile(r"\bfor\s*\(\s*(?:float|double)\b")
CAST_INT_RE = re.compile(
    r"(?:static_cast|reinterpret_cast|const_cast)\s*<\s*"
    r"(?:int|unsigned|uint32_t|int32_t|int16_t|uint16_t|long|size_t|u32|s32|u16|s16)\s*>"
    r"\s*\("
    r"|\(\s*(?:int|unsigned|uint32_t|int32_t|int16_t|uint16_t|long|size_t|u32|s32|u16|s16)\s*\)"
)
DOUBLE_TYPE_RE = re.compile(r"\b(long\s+double|double)\b")
UNSUFFIXED_FLOAT_RE = re.compile(r"(?<![\w.])(?:\d+\.\d+|\d+\.|\.\d+)(?![eE][+-]?\d*)(?![fF\w.])")
LIBM_RE = re.compile(
    r"\b(?:std::)?(?:sinf?|cosf?|tanf?|atan2f?|expf?|logf?|powf?)\s*\("
)
SQRT_RE = re.compile(r"\b(?:std::)?sqrtf?\s*\(")
FAST_TRIG_RE = re.compile(r"\b(?:fastSin|fastCos|fastAtan2)\b")
FX_IDENT_RE = re.compile(r"\b([A-Za-z_]\w*(?:_fx|_q16|_q8)|fx_[A-Za-z_]\w*)\b")
FLOAT_DECL_RE = re.compile(
    r"\b(?:float|double)\s+(?:const\s+)?(?P<names>[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)"
)
KEYWORD_STOP = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "static",
    "const",
    "void",
    "int",
    "float",
    "double",
    "bool",
    "char",
    "namespace",
    "class",
    "struct",
    "enum",
    "typedef",
    "template",
    "using",
    "inline",
    "extern",
    "public",
    "private",
    "protected",
    "virtual",
    "override",
    "operator",
    "new",
    "delete",
    "this",
    "true",
    "false",
    "nullptr",
    "NULL",
    "auto",
    "case",
    "default",
    "else",
    "do",
    "break",
    "continue",
    "goto",
}
ARCH_REQUIRED = ("-march=armv6k", "-mfloat-abi=hard", "-mfpu=vfp")
R1_THREAD_SITES = (
    "t2k_3ds/src/platform_3ds/main_3ds.cpp",
    "t2k_3ds/src/audio/music_3ds.cpp",
    "t2k_3ds/src/audio/sfx_3ds.cpp",
    "t2k_3ds/src/rendering/renderer_c3d.cpp",
    "t2k_core/src/audio/audio_thread.h",
)
STANDING_EXEMPTIONS: tuple[dict[str, object], ...] = (
    {
        "id": "E-LUT-1",
        "file": "t2k_core/src/game/math_lut.h",
        "symbols": ("fastSin",),
        "rules": ("R2", "R3", "R5"),
    },
    {
        "id": "E-LUT-2",
        "file": "t2k_core/src/game/math_lut.h",
        "symbols": ("fastAtan2",),
        "rules": ("R3", "R7"),
    },
    {
        "id": "E-LUT-3",
        "file": "t2k_core/src/game/math_lut.cpp",
        "symbols": ("mathLutInit",),
        "rules": ("R4", "R6"),
    },
    {
        "id": "E-MECH-1",
        "file": "t2k_core/src/game/engine.cpp",
        "symbols": ("*",),
        "rules": ("R4", "R6"),
    },
    {
        "id": "E-TEX-1",
        "file": "t2k_core/src/rendering/textures.cpp",
        "symbols": ("*",),
        "rules": ("R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10"),
    },
    {
        "id": "E-FFT-1",
        "file": "t2k_core/src/audio/fft.cpp",
        "symbols": ("*",),
        "rules": ("R6", "R7"),
    },
)


@dataclass
class Finding:
    rule: str
    severity: Severity
    file: str
    line: int
    symbol: str
    hotpath: bool
    hotpath_reason: str
    exemption: str | None
    detail: str


@dataclass
class AuditResult:
    status: Status
    blockers: int
    reviews: int
    findings: list[Finding] = field(default_factory=list)
    hotpath_source: str = "seed"
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, object]:
        return {
            "status": self.status,
            "blockers": self.blockers,
            "reviews": self.reviews,
            "hotpath_source": self.hotpath_source,
            "notes": self.notes,
            "findings": [asdict(f) for f in self.findings],
        }


@dataclass
class Function:
    name: str
    start: int
    end: int
    body: str
    line: int
    header: str


@dataclass
class Exemption:
    rules: frozenset[str]
    reason: str
    measured: str | None
    verified: str
    standing: bool
    line: int
    applies_to: str


@dataclass
class HotEntry:
    rel: str
    symbols: tuple[str, ...]
    why: str
    source: str


def repo_rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def is_excluded(path: Path) -> bool:
    resolved = path.resolve()
    return any(_is_under(resolved, prefix) for prefix in EXCLUDED_PREFIXES)


def _is_under(path: Path, prefix: Path) -> bool:
    try:
        path.relative_to(prefix.resolve())
        return True
    except ValueError:
        return False


def iter_cxx_files(root: Path | None = None) -> Iterator[Path]:
    roots = (root,) if root is not None else SRC_ROOTS
    for base in roots:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in CXX_EXTS:
                continue
            if is_excluded(path):
                continue
            yield path


def load_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_corpus(path: Path) -> dict[str, object]:
    if not path.exists():
        return {"version": 1, "seed": [], "cold_always": []}
    data = load_json(path)
    if not isinstance(data, dict):
        raise ValueError(f"corpus is not an object: {path}")
    return data


def strip_comments_keep_lines(text: str) -> str:
    out: list[str] = []
    i = 0
    n = len(text)
    in_str = False
    in_char = False
    in_line = False
    in_block = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if ch == "\n":
                in_line = False
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            out.append(ch)
            if ch == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if ch == "'":
                in_char = False
            i += 1
            continue
        if ch == "/" and nxt == "/":
            in_line = True
            out.append("  ")
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block = True
            out.append("  ")
            i += 2
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "'":
            in_char = True
            out.append(ch)
            i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def match_braces(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    in_str = False
    in_char = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_str:
            if ch == "\\" and nxt:
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            if ch == "\\" and nxt:
                i += 2
                continue
            if ch == "'":
                in_char = False
            i += 1
            continue
        if ch == '"':
            in_str = True
        elif ch == "'":
            in_char = True
        elif ch == "/" and nxt == "/":
            nl = text.find("\n", i)
            i = n if nl < 0 else nl
            continue
        elif ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


def parse_functions(text: str) -> list[Function]:
    cleaned = strip_comments_keep_lines(text)
    functions: list[Function] = []
    for match in FUNC_RE.finditer(cleaned):
        name = match.group("name")
        if name in KEYWORD_STOP:
            continue
        brace = match.end() - 1
        if cleaned[brace] != "{":
            continue
        end = match_braces(cleaned, brace)
        start = match.start()
        functions.append(
            Function(
                name=name,
                start=start,
                end=end + 1,
                body=cleaned[brace + 1 : end],
                line=line_of(text, start),
                header=cleaned[start:brace],
            )
        )
    return functions


def parse_exemptions(text: str) -> list[Exemption]:
    found: list[Exemption] = []
    for match in EXEMPT_RE.finditer(text):
        rules = frozenset(part.strip() for part in match.group("rules").split(","))
        reason = re.sub(r"\s+", " ", match.group("reason")).strip()
        measured = match.group("measured")
        standing = "standing" in reason.lower()
        found.append(
            Exemption(
                rules=rules,
                reason=reason,
                measured=measured,
                verified=match.group("verified"),
                standing=standing,
                line=line_of(text, match.start()),
                applies_to="",
            )
        )
    return found


def exemption_for_index(text: str, exemptions: Sequence[Exemption], index: int) -> Exemption | None:
    best: Exemption | None = None
    best_dist = 10**9
    for exemption in exemptions:
        pos = 0
        for _ in range(exemption.line - 1):
            nxt = text.find("\n", pos)
            if nxt < 0:
                pos = len(text)
                break
            pos = nxt + 1
        if pos <= index and (index - pos) < best_dist:
            best = exemption
            best_dist = index - pos
    if best is None:
        return None
    if best_dist > 800:
        return None
    return best


def split_loops(body: str) -> list[tuple[int, str]]:
    loops: list[tuple[int, str]] = []
    for match in re.finditer(r"\b(for|while|do)\b", body):
        i = match.end()
        while i < len(body) and body[i] in " \t\n":
            i += 1
        if i < len(body) and body[i] == "(":
            depth = 0
            j = i
            while j < len(body):
                if body[j] == "(":
                    depth += 1
                elif body[j] == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            i = j
            while i < len(body) and body[i] in " \t\n":
                i += 1
        if i < len(body) and body[i] == "{":
            end = match_braces(body, i)
            loops.append((match.start(), body[i + 1 : end]))
        else:
            semi = body.find(";", i)
            if semi >= 0:
                loops.append((match.start(), body[i : semi + 1]))
    return loops


def float_names_in(text: str) -> set[str]:
    names: set[str] = set()
    for match in FLOAT_DECL_RE.finditer(text):
        for raw in match.group("names").split(","):
            ident = raw.strip()
            if ident:
                names.add(ident)
    return names


def looks_float_expr(expr: str, float_ids: set[str]) -> bool:
    if UNSUFFIXED_FLOAT_RE.search(expr) or re.search(r"\d+(?:\.\d*)?[fF]\b", expr):
        return True
    if re.search(r"\b(?:float|double)\b", expr):
        return True
    tokens = set(re.findall(r"[A-Za-z_]\w*", expr))
    if tokens & float_ids:
        return True
    if FAST_TRIG_RE.search(expr):
        return True
    return False


def looks_int_expr(expr: str) -> bool:
    stripped = expr.strip()
    if re.fullmatch(r"\d+", stripped):
        return True
    if re.search(r"\b(?:int|unsigned|size_t|uint32_t|int32_t|u32|s32)\b", expr):
        return True
    return False


def condition_is_float(cond: str, float_ids: set[str]) -> bool:
    parts = re.split(r"(&&|\|\|)", cond)
    for part in parts:
        if re.search(r"(==|!=|<=|>=|<|>)", part) and looks_float_expr(part, float_ids):
            if looks_int_expr(part) and not looks_float_expr(part, float_ids):
                continue
            return True
        if part.strip().startswith("!") and looks_float_expr(part, float_ids):
            return True
    return looks_float_expr(cond, float_ids) and bool(
        re.search(r"(==|!=|<=|>=|<|>)", cond)
    )


def standing_covers(rel: str, symbol: str, rule: str) -> str | None:
    rel_n = rel.replace("\\", "/")
    base = rel_n.rsplit("/", 1)[-1]
    for item in STANDING_EXEMPTIONS:
        file_n = str(item["file"]).replace("\\", "/")
        file_base = file_n.rsplit("/", 1)[-1]
        path_ok = (
            rel_n == file_n
            or rel_n.endswith("/" + file_n)
            or (
                file_base in {"math_lut.h", "math_lut.cpp"}
                and base == file_base
            )
        )
        if not path_ok:
            continue
        symbols = item["symbols"]
        assert isinstance(symbols, tuple)
        if "*" not in symbols and symbol not in symbols:
            continue
        rules = item["rules"]
        assert isinstance(rules, tuple)
        if rule in rules:
            return str(item["id"])
    return None


def cold_file(rel: str, cold: Sequence[str]) -> bool:
    rel_n = rel.replace("\\", "/")
    for prefix in cold:
        p = prefix.replace("\\", "/")
        if rel_n == p or rel_n.startswith(p.rstrip("/") + "/"):
            return True
    return False


def is_init_symbol(name: str) -> bool:
    return bool(INIT_NAME_RE.search(name))


def classify_hot(
    rel: str,
    func: Function,
    seed: Mapping[str, HotEntry],
    annotated: bool,
    cold: Sequence[str],
) -> tuple[bool, str]:
    if cold_file(rel, cold):
        return False, ""
    if annotated:
        return True, "@hotpath"
    entry = seed.get(rel)
    if entry is None:
        return False, ""
    if "*" in entry.symbols:
        if is_init_symbol(func.name):
            return False, ""
        return True, entry.source
    if func.name in entry.symbols:
        return True, entry.source
    return False, ""


def seed_map(corpus: Mapping[str, object], extra: Sequence[HotEntry] | None = None) -> dict[str, HotEntry]:
    out: dict[str, HotEntry] = {}
    raw_seed = corpus.get("seed", [])
    if isinstance(raw_seed, list):
        for item in raw_seed:
            if not isinstance(item, dict):
                continue
            file_name = str(item.get("file", ""))
            symbols_raw = item.get("symbols", [])
            symbols = tuple(str(s) for s in symbols_raw) if isinstance(symbols_raw, list) else ("*",)
            why = str(item.get("why", "seed"))
            out[file_name] = HotEntry(file_name, symbols, why, "seed")
    if extra:
        for item in extra:
            out[item.rel] = item
    return out


def ingest_perf_logs(docs_dir: Path) -> tuple[list[HotEntry], str]:
    if not docs_dir.exists():
        return [], "seed"
    logs = sorted(docs_dir.glob("perf-*.log"))
    if not logs:
        return [], "seed"
    stage_map = {
        "grid": [
            "t2k_core/src/rendering/grid_geometry.cpp",
            "t2k_3ds/src/rendering/renderer_c3d.cpp",
        ],
        "line": ["t2k_core/src/rendering/line_geometry.cpp"],
        "fx": [
            "t2k_core/src/rendering/shatter.cpp",
            "t2k_core/src/rendering/entity_geometry.cpp",
        ],
        "ml": ["t2k_core/src/rendering/shatter.cpp"],
        "part": ["t2k_core/src/rendering/entity_geometry.cpp"],
        "cpu_busy": ["t2k_core/src/game/game_step.cpp"],
        "build": ["t2k_core/src/game/game_step.cpp"],
        "issue": ["t2k_3ds/src/rendering/renderer_c3d.cpp"],
    }
    averages: dict[str, list[float]] = {}
    stage_re = re.compile(
        r"\b(grid|line|fx|ml|part|cpu_busy|build|issue)\b[^\d]{0,12}(\d+(?:\.\d+)?)"
    )
    for log in logs[-8:]:
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in stage_re.finditer(text):
            averages.setdefault(match.group(1), []).append(float(match.group(2)))
    if not averages:
        return [], "seed"
    ranked = sorted(
        ((name, sum(vals) / len(vals)) for name, vals in averages.items()),
        key=lambda kv: kv[1],
        reverse=True,
    )[:15]
    extra: list[HotEntry] = []
    for name, _avg in ranked:
        for rel in stage_map.get(name, []):
            extra.append(HotEntry(rel, ("*",), f"profile:{name}", f"profile:{name}"))
    return extra, "profile"


def gate_r10(makefile: Path) -> list[Finding]:
    findings: list[Finding] = []
    if not makefile.exists():
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(makefile),
                0,
                "ARCH",
                True,
                "seed",
                None,
                "t2k_3ds/Makefile missing",
            )
        )
        return findings
    text = makefile.read_text(encoding="utf-8")
    arch_line = ""
    arch_line_no = 0
    flags_blob = text
    for i, line in enumerate(text.splitlines(), 1):
        if line.startswith("ARCH"):
            arch_line = line
            arch_line_no = i
            break
    missing = [flag for flag in ARCH_REQUIRED if flag not in arch_line]
    if missing:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(makefile),
                arch_line_no,
                "ARCH",
                True,
                "seed",
                None,
                "ARCH missing " + ", ".join(missing),
            )
        )
    if "-fno-math-errno" not in flags_blob:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(makefile),
                0,
                "COMMONFLAGS",
                True,
                "seed",
                None,
                "COMMONFLAGS/CXXFLAGS missing -fno-math-errno",
            )
        )
    if re.search(r"-mfloat-abi=(softfp|soft)\b", text):
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(makefile),
                0,
                "ARCH",
                True,
                "seed",
                None,
                "soft/softfp float ABI present",
            )
        )
    return findings


def probe_objects(build_dir: Path) -> list[Finding]:
    findings: list[Finding] = []
    if not build_dir.exists():
        return findings
    objects = list(build_dir.glob("*.o"))
    if not objects:
        return findings
    readelf = shutil.which("arm-none-eabi-readelf") or shutil.which("readelf")
    if not readelf:
        return findings
    sample = objects[0]
    try:
        proc = subprocess.run(
            [readelf, "-A", str(sample)],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return findings
    blob = proc.stdout + proc.stderr
    if "Tag_ABI_VFP_args" not in blob:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(sample),
                0,
                "object",
                True,
                "seed",
                None,
                "object missing Tag_ABI_VFP_args",
            )
        )
    if "VFP" not in blob and "Tag_FP_arch" not in blob:
        findings.append(
            Finding(
                "R10",
                "MUST",
                repo_rel(sample),
                0,
                "object",
                True,
                "seed",
                None,
                "object missing VFP Tag_FP_arch",
            )
        )
    return findings


def fpscr_ok(text: str) -> bool:
    if FPSCR_HELPER_RE.search(text):
        return True
    if FPSCR_ASM_RE.search(text) and re.search(r"\b(FZ|DN|0x0[23]000000|1\s*<<\s*24|1\s*<<\s*25)\b", text):
        return True
    if re.search(r"vmsr\s+FPSCR", text, re.IGNORECASE) and re.search(
        r"(1u?\s*<<\s*24).*(1u?\s*<<\s*25)|(1u?\s*<<\s*25).*(1u?\s*<<\s*24)|0x03000000",
        text,
    ):
        return True
    return False


def _fpscr_comment_ok(text: str) -> bool:
    lowered = text.lower()
    has_denormal = "denormal" in lowered
    has_fz_dn = ("flush-to-zero" in lowered or "flush to zero" in lowered) or (
        "fz" in lowered and "dn" in lowered
    )
    has_per_thread = (
        "per-thread" in lowered
        or "per thread" in lowered
        or "on this thread" in lowered
        or "each worker" in lowered
        or "every thread" in lowered
    )
    return (has_denormal or has_fz_dn) and has_per_thread


def window_after(text: str, index: int, lines: int = 40) -> str:
    start = index
    consumed = 0
    i = index
    while i < len(text) and consumed < lines:
        if text[i] == "\n":
            consumed += 1
        i += 1
    return text[start:i]


def gate_r1(root: Path | None = None) -> list[Finding]:
    findings: list[Finding] = []
    base = root or REPO_ROOT
    any_fpscr = False
    for rel in R1_THREAD_SITES:
        path = base / rel
        if not path.exists():
            findings.append(
                Finding(
                    "R1",
                    "MUST",
                    rel,
                    0,
                    "missing",
                    True,
                    "seed",
                    None,
                    "required thread site missing",
                )
            )
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if fpscr_ok(text):
            any_fpscr = True
            if not _fpscr_comment_ok(text):
                findings.append(
                    Finding(
                        "R1",
                        "MUST",
                        rel,
                        0,
                        "fpscr-comment",
                        True,
                        "seed",
                        None,
                        "FPSCR code without VFPU rationale comment (denormal/FZ|DN, per-thread)",
                    )
                )
        if rel.endswith("main_3ds.cpp") and not fpscr_ok(text):
            findings.append(
                Finding(
                    "R1",
                    "MUST",
                    rel,
                    0,
                    "main",
                    True,
                    "seed",
                    None,
                    "no FPSCR FZ|DN write on main or workers",
                )
            )
        for match in THREAD_CREATE_RE.finditer(text):
            chunk = window_after(text, match.start(), 40)
            if not fpscr_ok(chunk) and not fpscr_ok(text):
                findings.append(
                    Finding(
                        "R1",
                        "MUST",
                        rel,
                        line_of(text, match.start()),
                        "threadCreate",
                        True,
                        "seed",
                        None,
                        "threadCreate without FPSCR FZ|DN within 40 lines and file has no helper",
                    )
                )
    if any_fpscr:
        findings = [
            f
            for f in findings
            if not (
                f.file.endswith("main_3ds.cpp")
                and f.detail == "no FPSCR FZ|DN write on main or workers"
            )
        ]
    if not any_fpscr and not any(f.file.endswith("main_3ds.cpp") for f in findings):
        findings.append(
            Finding(
                "R1",
                "MUST",
                "t2k_3ds/src/platform_3ds/main_3ds.cpp",
                0,
                "main",
                True,
                "seed",
                None,
                "no FPSCR FZ|DN write on main or workers",
            )
        )
    return _dedupe(findings)


def _dedupe(findings: Sequence[Finding]) -> list[Finding]:
    seen: set[tuple[object, ...]] = set()
    out: list[Finding] = []
    for item in findings:
        key = (
            item.rule,
            item.file,
            item.line,
            item.symbol,
            item.detail,
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(item)
    return out


def gate_exemptions(text: str, rel: str) -> list[Finding]:
    findings: list[Finding] = []
    for match in re.finditer(r"/\*\s*@vfp-exempt\b", text):
        chunk = text[match.start() : match.start() + 400]
        parsed = EXEMPT_RE.match(chunk)
        if parsed is None:
            findings.append(
                Finding(
                    "R-EXEMPT",
                    "MUST",
                    rel,
                    line_of(text, match.start()),
                    "@vfp-exempt",
                    True,
                    "@hotpath",
                    None,
                    "invalid @vfp-exempt grammar",
                )
            )
            continue
        rules = [part.strip() for part in parsed.group("rules").split(",")]
        reason = parsed.group("reason")
        measured = parsed.group("measured")
        verified = parsed.group("verified")
        standing = "standing" in reason.lower()
        must_rules = [r for r in rules if r not in {"R7", "R9"}]
        if must_rules and not measured and not standing:
            findings.append(
                Finding(
                    "R-EXEMPT",
                    "MUST",
                    rel,
                    line_of(text, match.start()),
                    "@vfp-exempt",
                    True,
                    "@hotpath",
                    None,
                    "MUST exemption missing measured cost",
                )
            )
        age = _age_days(verified)
        if age is not None and age > PIN_MAX_AGE_DAYS:
            findings.append(
                Finding(
                    "R-EXPIRED",
                    "MUST",
                    rel,
                    line_of(text, match.start()),
                    "@vfp-exempt " + ",".join(rules),
                    True,
                    "@hotpath",
                    None,
                    f"exemption verified {verified}, {age}d old > {PIN_MAX_AGE_DAYS}d "
                    "-- re-verify against a current OG-profile capture, then bump the date",
                )
            )
    return findings


def scan_function(
    rel: str,
    func: Function,
    text: str,
    exemptions: Sequence[Exemption],
    hot_reason: str,
) -> list[Finding]:
    findings: list[Finding] = []
    float_ids = float_names_in(func.header + "\n" + func.body)
    loops = split_loops(func.body)

    def maybe(
        rule: str,
        severity: Severity,
        rel_off: int,
        detail: str,
        *,
        untyped: bool = False,
    ) -> None:
        abs_index = func.start + rel_off
        line = line_of(text, abs_index) if 0 <= abs_index < len(text) else func.line
        cover = standing_covers(rel, func.name, rule)
        local = exemption_for_index(text, exemptions, abs_index)
        if local and rule in local.rules:
            cover = cover or "inline"
        if cover:
            return
        sev: Severity = "SHOULD" if untyped and severity == "MUST" else severity
        findings.append(
            Finding(
                rule,
                sev,
                rel,
                line,
                func.name,
                True,
                hot_reason,
                None,
                detail if not untyped else f"untyped-heuristic: {detail}",
            )
        )

    for _off, loop in loops:
        loop_abs = func.body.find(loop) if loop else 0
        for match in CAST_INT_RE.finditer(loop):
            expr = loop[match.end() : match.end() + 80]
            untyped = not looks_float_expr(expr, float_ids) and "." not in expr and "float" not in expr
            if looks_int_expr(expr) and not looks_float_expr(expr, float_ids):
                continue
            maybe("R2", "MUST", max(loop_abs, 0) + match.start(), "float-to-int cast in loop body", untyped=untyped)

        for match in re.finditer(r"\b(if|while|for)\s*\(", loop):
            start = match.end() - 1
            depth = 0
            j = start
            while j < len(loop):
                if loop[j] == "(":
                    depth += 1
                elif loop[j] == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            cond = loop[start:j]
            if match.group(1) == "for":
                parts = cond.strip("()").split(";")
                cond_expr = parts[1] if len(parts) >= 2 else cond
            else:
                cond_expr = cond
            if condition_is_float(cond_expr, float_ids):
                maybe(
                    "R3",
                    "MUST",
                    max(loop_abs, 0) + match.start(),
                    "float comparison driving control flow in hot loop",
                    untyped=not (float_ids or UNSUFFIXED_FLOAT_RE.search(cond_expr) or re.search(r"[fF]\b", cond_expr)),
                )

        for match in re.finditer(r"\?[^;]+?:", loop):
            if looks_float_expr(match.group(0), float_ids):
                maybe(
                    "R3",
                    "MUST",
                    max(loop_abs, 0) + match.start(),
                    "float ternary in hot loop",
                )

        if FOR_FLOAT_RE.search(loop) or FOR_FLOAT_RE.search(func.body[_off : _off + 80] if _off >= 0 else ""):
            maybe("R5", "MUST", _off, "float-typed induction variable")

        for match in re.finditer(r"([A-Za-z_]\w*)\s*\[([^\]]+)\]", loop):
            idx = match.group(2)
            if looks_float_expr(idx, float_ids) and not looks_int_expr(idx):
                maybe("R5", "MUST", max(loop_abs, 0) + match.start(), "float value used as array subscript")

        for match in re.finditer(r"(?<![=\!<>])/(?![/\*=])", loop):
            left = loop[max(0, match.start() - 40) : match.start()]
            right = loop[match.end() : match.end() + 40]
            if looks_float_expr(left, float_ids) or looks_float_expr(right, float_ids):
                if looks_int_expr(left) and looks_int_expr(right) and not looks_float_expr(left + right, float_ids):
                    continue
                maybe("R7", "SHOULD", max(loop_abs, 0) + match.start(), "float division in hot loop")

        for match in SQRT_RE.finditer(loop):
            maybe("R7", "SHOULD", max(loop_abs, 0) + match.start(), "sqrtf in hot loop")

    whole = func.header + "\n" + func.body
    if DOUBLE_TYPE_RE.search(whole):
        match = DOUBLE_TYPE_RE.search(whole)
        assert match is not None
        maybe("R4", "MUST", match.start(), "double/long double in hot path")
    for match in UNSUFFIXED_FLOAT_RE.finditer(whole):
        maybe("R4", "MUST", match.start(), "unsuffixed float literal promotes to double")
    for match in LIBM_RE.finditer(whole):
        maybe("R6", "MUST", match.start(), "libm transcendental in hot path")
    for match in FX_IDENT_RE.finditer(whole):
        window = whole[match.start() : match.start() + 60]
        if re.search(r"[+\-*/]", window) and looks_float_expr(window, float_ids):
            maybe("R8", "SHOULD", match.start(), "fixed-point identifier mixed with float")

    if FOR_FLOAT_RE.search(func.header + func.body[:120]):
        maybe("R5", "MUST", 0, "float-typed induction variable")

    loop_spans = []
    for off, loop in loops:
        loop_spans.append((off, off + len(loop) + 8))
    for match in re.finditer(r"(?<![=\!<>])/(?![/\*=])", func.body):
        if any(start <= match.start() <= end for start, end in loop_spans):
            continue
        left = func.body[max(0, match.start() - 40) : match.start()]
        right = func.body[match.end() : match.end() + 40]
        if looks_float_expr(left, float_ids) or looks_float_expr(right, float_ids):
            if looks_int_expr(left) and looks_int_expr(right) and not looks_float_expr(left + right, float_ids):
                continue
            maybe("R7", "SHOULD", match.start(), "float division inner_loop=false")
    for match in SQRT_RE.finditer(func.body):
        if any(start <= match.start() <= end for start, end in loop_spans):
            continue
        maybe("R7", "SHOULD", match.start(), "sqrtf inner_loop=false")

    return findings


def scan_file(
    path: Path,
    seed: Mapping[str, HotEntry],
    cold: Sequence[str],
    *,
    force_hot: bool = False,
) -> list[Finding]:
    rel = repo_rel(path) if _is_under(path.resolve(), REPO_ROOT) else path.name
    if force_hot:
        rel_key = rel
    else:
        rel_key = repo_rel(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    findings = gate_exemptions(text, rel_key)
    exemptions = parse_exemptions(text)
    annotated_lines = {line_of(text, m.start()) for m in HOTPATH_ANN_RE.finditer(text)}
    functions = parse_functions(text)
    for func in functions:
        annotated = any(func.line - 3 <= ln <= func.line for ln in annotated_lines)
        hot, reason = classify_hot(rel_key, func, seed, annotated or force_hot, cold)
        if force_hot:
            hot, reason = True, reason or "seed"
        if not hot:
            continue
        findings.extend(scan_function(rel_key, func, text, exemptions, reason))
    if force_hot and not functions:
        dummy = Function("body", 0, len(text), strip_comments_keep_lines(text), 1, "")
        findings.extend(scan_function(rel_key, dummy, text, exemptions, "seed"))
    return findings


def scan_tree(
    root: Path,
    corpus: Mapping[str, object],
    *,
    ingest_profile: bool = True,
) -> tuple[list[Finding], str]:
    extra: list[HotEntry] = []
    source = "seed"
    if ingest_profile:
        extra, source = ingest_perf_logs(root / "docs" / "validation")
    seed = seed_map(corpus, extra)
    cold_raw = corpus.get("cold_always", [])
    cold = [str(x) for x in cold_raw] if isinstance(cold_raw, list) else []
    findings: list[Finding] = []
    src_roots = (root / "t2k_core" / "src", root / "t2k_3ds" / "src")
    files: list[Path] = []
    for base in src_roots:
        if base.exists():
            files.extend(p for p in base.rglob("*") if p.is_file() and p.suffix in CXX_EXTS)
    targets = {entry.rel for entry in seed.values()}
    for path in files:
        rel = path.resolve().relative_to(root.resolve()).as_posix()
        if rel not in targets and not any(rel.startswith(c.rstrip("/") + "/") for c in targets):
            if not HOTPATH_ANN_RE.search(path.read_text(encoding="utf-8", errors="replace")):
                continue
        findings.extend(scan_file(path, seed, cold))
    return findings, source


def gate_r9(build_dir: Path) -> list[Finding]:
    if os.environ.get("VFP_OBJDUMP") != "1":
        return []
    if not build_dir.exists():
        return []
    objdump = shutil.which("arm-none-eabi-objdump")
    if not objdump:
        return []
    findings: list[Finding] = []
    s_reg = re.compile(r"\bs([0-9]|[12][0-9]|3[01])\b")
    for obj in sorted(build_dir.glob("*.o"))[:20]:
        try:
            proc = subprocess.run(
                [objdump, "-d", str(obj)],
                check=False,
                capture_output=True,
                text=True,
                timeout=20,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        live = {m.group(0) for m in s_reg.finditer(proc.stdout)}
        if len(live) > 24:
            findings.append(
                Finding(
                    "R9",
                    "SHOULD",
                    repo_rel(obj),
                    0,
                    obj.stem,
                    True,
                    "seed",
                    None,
                    f"live VFP s-regs {len(live)} > 24",
                )
            )
    return findings


def gate_corpus_freshness(corpus: Mapping[str, object], docs_dir: Path) -> list[Finding]:
    """The corpus is a MEASUREMENT, not a preference list.

    AGENTS.md 2 defines hot as "top 15 of a captured profile" and warns that a
    hand-maintained list silently stops covering the code. That warning only
    bites if the capture has a date attached and that date is checked: an
    undated corpus reads as current forever, which is exactly the decay the rule
    describes. The newest-log cross-check exists so the date cannot be bumped
    without a capture actually landing on disk.
    """
    findings: list[Finding] = []
    corpus_rel = repo_rel(DEFAULT_CORPUS)
    captured = corpus.get("captured")
    if not isinstance(captured, str) or not captured.strip():
        findings.append(
            Finding(
                "R-CORPUS",
                "MUST",
                corpus_rel,
                0,
                "hotpath_corpus",
                True,
                "corpus",
                None,
                "corpus declares no `captured` date -- hot is a measurement "
                "(AGENTS.md 2), so the corpus must say when it was measured",
            )
        )
        return findings
    age = _age_days(captured)
    if age is not None and age > CORPUS_MAX_AGE_DAYS:
        findings.append(
            Finding(
                "R-CORPUS",
                "MUST",
                corpus_rel,
                0,
                "hotpath_corpus",
                True,
                "corpus",
                None,
                f"hot-path corpus captured {captured}, {age}d old > "
                f"{CORPUS_MAX_AGE_DAYS}d -- 'top 15 of a captured profile' is "
                "meaningless against a stale capture; re-capture and regenerate",
            )
        )
    newest = ""
    if docs_dir.exists():
        for log in docs_dir.glob("perf-*.log"):
            stamp = re.search(r"(\d{4}-\d{2}-\d{2})", log.name)
            if stamp and stamp.group(1) > newest:
                newest = stamp.group(1)
    if newest and captured > newest:
        findings.append(
            Finding(
                "R-CORPUS",
                "SHOULD",
                corpus_rel,
                0,
                "hotpath_corpus",
                True,
                "corpus",
                None,
                f"corpus claims captured {captured} but the newest perf log on "
                "disk is {0} -- the date was bumped without a capture".format(newest),
            )
        )
    return findings


def run_audit(
    root: Path | None = None,
    *,
    corpus_path: Path | None = None,
    makefile: Path | None = None,
    ingest_profile: bool = True,
    scan_sources: bool = True,
) -> AuditResult:
    root = root or REPO_ROOT
    corpus = load_corpus(corpus_path or DEFAULT_CORPUS)
    findings: list[Finding] = []
    notes: list[str] = []
    findings.extend(gate_r10(makefile or (root / "t2k_3ds" / "Makefile")))
    findings.extend(probe_objects(root / "t2k_3ds" / "build"))
    findings.extend(gate_r1(root))
    findings.extend(gate_corpus_freshness(corpus, root / "docs" / "validation"))
    source = "seed"
    if scan_sources:
        scanned, source = scan_tree(root, corpus, ingest_profile=ingest_profile)
        findings.extend(scanned)
    r9 = gate_r9(root / "t2k_3ds" / "build")
    findings.extend(r9)
    if os.environ.get("VFP_OBJDUMP") != "1":
        notes.append("R9 omitted (VFP_OBJDUMP!=1)")
    elif not (root / "t2k_3ds" / "build").exists():
        notes.append("R9 omitted (no t2k_3ds/build/*.o)")
    findings = _dedupe(findings)
    blockers = sum(1 for f in findings if f.severity == "MUST")
    reviews = sum(1 for f in findings if f.severity == "SHOULD")
    status: Status = "FAIL" if blockers else "PASS"
    return AuditResult(status, blockers, reviews, findings, source, notes)


def write_result(result: AuditResult, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(result.to_json(), indent=2) + "\n", encoding="utf-8")


def _cli(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="gates_vfp", description="VFP hot-path gates R1–R10")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--makefile", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--no-profile", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    result = run_audit(
        args.root,
        corpus_path=args.corpus,
        makefile=args.makefile,
        ingest_profile=not args.no_profile,
    )
    write_result(result, args.out)
    if args.json:
        json.dump(result.to_json(), sys.stdout, indent=2)
        sys.stdout.write("\n")
    else:
        print(f"{result.status} blockers={result.blockers} reviews={result.reviews}")
        for finding in result.findings:
            print(
                f"{finding.rule} {finding.severity} {finding.file}:{finding.line} "
                f"{finding.symbol} {finding.detail}"
            )
    return 3 if result.status == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(_cli())
