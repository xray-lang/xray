#!/usr/bin/env python3
"""Measured VM and AOT results for every standard library module probe.

The boundary manifest records the policy a module is meant to follow and the
contract probes state what its public surface should do. Neither says whether
the two backends can execute that surface, so no policy report can separate a
module the AOT path compiles from one it refuses.

This probe answers that by measurement alone. For every module named in the
manifest it runs the module's contract probe on the VM, asks the AOT path for
generated C, and -- when C is produced -- regenerates it to compare the two
outputs byte for byte. No result is derived from a label.

A program that imports nothing is measured first as a control. When the AOT
path refuses that program the toolchain itself is broken and no module result
means anything, so the report sets `baseline_ok` false and the exit status is
non-zero. The exit status is also non-zero when a manifest module has no
probe, because manifest and corpus then disagree. AOT refusals of the modules
themselves are the measurement rather than a failure of this probe and leave
the exit status at zero.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stdlib_manifest import load_manifest  # noqa: E402


SCHEMA = 2

CURRENT_PSC_SCHEMA = 9
CURRENT_SEMANTIC_SCHEMA = 45
CURRENT_TARGET_SCHEMA = 55
CURRENT_XTP_SCHEMA = 55

SCHEMA_DEFINES = {
    "psc_schema": (
        Path("src/plan/semantic/xr_program_semantic_closure.h"),
        "XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION",
        CURRENT_PSC_SCHEMA,
    ),
    "semantic_schema": (
        Path("src/plan/semantic/xr_semantic_ids.h"),
        "XR_SEMANTIC_SCHEMA_VERSION",
        CURRENT_SEMANTIC_SCHEMA,
    ),
    "target_schema": (
        Path("src/plan/target/xr_target_profile.h"),
        "XR_TARGET_PLAN_SCHEMA_VERSION",
        CURRENT_TARGET_SCHEMA,
    ),
    "xtp_schema": (
        Path("src/plan/format/xr_xtp_schema.h"),
        "XR_XTP_SCHEMA_VERSION",
        CURRENT_XTP_SCHEMA,
    ),
}

MODULE_SUMMARY_RE = re.compile(
    r"\[module-summary\] modules=(\d+) graph=([0-9a-f]{64})\b"
)
PROGRAM_CLOSURE_RE = re.compile(
    r"\[program-closure\] modules=(\d+) dependencies=(\d+) "
    r"psc=([0-9a-f]{64}) gci=([0-9a-f]{32})\b"
)
TARGET_PLAN_RE = re.compile(
    r"^target-plan module=(\d+) schema=(\d+) "
    r"fingerprint=([0-9a-f]{64}) semantic=([0-9a-f]{64})\b",
    re.MULTILINE,
)

PROBE_ROOT = Path("tests/stdlib/contracts")
PROBE_RELATIVE = Path("probes/current.xr")

WORKSPACE_PLACEHOLDER = "<probe-workspace>"

# Captured streams are evidence, not archives: a refusal states itself in its
# opening lines, so a bounded prefix keeps the report readable while the full
# text still backs the extracted diagnostic.
CAPTURE_CHARS = 4000

# The control program imports nothing, so every refusal it draws belongs to
# the toolchain rather than to a standard library module.
BASELINE_SOURCE = """fn main() -> i32 {
    print("baseline")
    return 0
}
"""

# Stage markers in pipeline order, each a literal the compiler prints. The
# pipeline stops at the first stage that refuses, so the earliest marker
# present in stderr names the refusing stage; the table is consulted in this
# order and the first hit wins.
#
# Module graph construction is reported as `parse` because it is the
# front-end step that acquires source, and the stage vocabulary has no
# separate resolution bucket. The `at <name>:` markers come from the Xi
# pipeline, which names its own failing phase in the message.
STAGE_MARKERS: tuple[tuple[str, str], ...] = (
    ("failed to parse module", "parse"),
    ("[module_graph] parse failed", "parse"),
    ("module graph build failed", "parse"),
    ("at analyze:", "semantic"),
    ("at lower:", "semantic"),
    ("at verify-raw:", "semantic"),
    ("at optimize:", "semantic"),
    ("at escape:", "semantic"),
    ("at ownership:", "semantic"),
    ("at semantic-plan:", "semantic"),
    ("semantic analysis failed", "semantic"),
    ("post-monomorphization analysis failed", "semantic"),
    ("at representation:", "target_plan"),
    ("at backend:", "target_plan"),
    ("TargetPlan build failed", "target_plan"),
    ("TargetPlan requires", "target_plan"),
    ("at emit:", "cgen"),
    ("C code generation failed", "cgen"),
    ("C source generation failed", "cgen"),
    ("C generation failed", "cgen"),
    ("linking failed", "link"),
    ("failed to build AOT link manifest", "link"),
    ("AOT manifest linking failed", "link"),
)

# Consulted only when no stage marker is present: a diagnostic code names the
# subsystem that issued it, which is weaker evidence than the stage the
# pipeline reported but better than no answer.
CODE_PREFIX_STAGES: tuple[tuple[str, str], ...] = (
    ("XR_SEM_", "semantic"),
    ("XR_TARGET_", "target_plan"),
    ("XR_CGEN_", "cgen"),
    ("XR_LINK_", "link"),
)

DIAGNOSTIC_CODE_RE = re.compile(r"\b(?:XR_[A-Z][A-Z0-9]*_[0-9]+|XI_[A-Z][A-Z0-9_]*)\b")
ERROR_MARKER_RE = re.compile(r"\b(?:error|Error|ERROR):")
WARNING_MARKER_RE = re.compile(r"\b(?:warning|Warning|WARNING)\b")


@dataclass
class CommandResult:
    """One compiler invocation, recorded as it behaved."""

    argv: list[str]
    returncode: int | None
    timed_out: bool
    stdout: str
    stderr: str
    stdout_truncated: bool
    stderr_truncated: bool


@dataclass
class GeneratedC:
    """Byte-level comparison of two generations of the same probe."""

    checked: bool
    identical: bool | None
    first_sha256: str
    second_sha256: str
    first_bytes: int | None
    second_bytes: int | None
    second_returncode: int | None
    note: str


@dataclass
class FirstRefusal:
    """The opening refusal of a failed AOT run."""

    diagnostic: str
    code: str
    stage: str


@dataclass(frozen=True)
class SchemaAuthority:
    """Exact source-owned schemas required by the current target machine."""

    psc_schema: int
    semantic_schema: int
    target_schema: int
    xtp_schema: int


@dataclass
class PlanObservation:
    """Backend plan facts observed without inventing an absent artifact.

    The AOT CLI exposes PSC/GCI and its in-memory program TargetPlan.  It does
    not emit an XTP artifact for this command, so ``xtp_verified`` remains
    false and the completion gate must not treat this partial observation as a
    unified VM/AOT plan identity.
    """

    psc_schema: int
    semantic_schema: int
    target_schema: int
    xtp_schema: int
    module_count: int
    dependency_count: int
    program_fingerprint: str
    module_set_fingerprint: str
    generation_closure_id: str
    semantic_fingerprint: str
    target_fingerprint: str
    xtp_sha256: str = ""
    xtp_verified: bool = False
    complete: bool = False
    errors: list[str] = field(default_factory=list)


@dataclass
class ModuleResult:
    """Both backend paths measured for one module."""

    module: str
    probe: str
    probe_present: bool
    vm: CommandResult | None
    aot: CommandResult | None
    c_bytes: int | None
    c_sha256: str
    generated_c: GeneratedC | None
    first_refusal: FirstRefusal | None
    vm_plan: PlanObservation | None
    aot_plan: PlanObservation | None


def source_schema_authority(root: Path) -> tuple[SchemaAuthority, list[str]]:
    """Read and hard-check the four current authority schemas from source."""
    values: dict[str, int] = {}
    errors: list[str] = []
    for field_name, (relative, define, expected) in SCHEMA_DEFINES.items():
        path = root / relative
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            values[field_name] = 0
            errors.append(f"{relative}: cannot read {define}: {exc}")
            continue
        match = re.search(
            rf"^\s*#define\s+{re.escape(define)}\s+UINT32_C\((\d+)\)\s*$",
            text,
            re.MULTILINE,
        )
        value = int(match.group(1)) if match else 0
        values[field_name] = value
        if not match:
            errors.append(f"{relative}: exact {define} definition is missing")
        elif value != expected:
            errors.append(
                f"{relative}: {define}={value}, expected current schema {expected}"
            )
    return SchemaAuthority(**values), errors


def parse_aot_plan_observation(
    stdout: str, schemas: SchemaAuthority
) -> PlanObservation:
    """Extract only identities the successful AOT invocation actually prints."""
    errors: list[str] = []
    summary = MODULE_SUMMARY_RE.search(stdout)
    closure = PROGRAM_CLOSURE_RE.search(stdout)
    targets = TARGET_PLAN_RE.findall(stdout)

    summary_modules = int(summary.group(1)) if summary else 0
    module_set = summary.group(2) if summary else ""
    module_count = int(closure.group(1)) if closure else 0
    dependency_count = int(closure.group(2)) if closure else 0
    program = closure.group(3) if closure else ""
    generation = closure.group(4) if closure else ""

    if not summary:
        errors.append("AOT output has no exact module-set fingerprint")
    if not closure:
        errors.append("AOT output has no PSC fingerprint or GCI")
    if summary and closure and summary_modules != module_count:
        errors.append(
            f"module summary count {summary_modules} != PSC module count {module_count}"
        )

    target_indices = {int(item[0]) for item in targets}
    target_schemas = {int(item[1]) for item in targets}
    target_fingerprints = {item[2] for item in targets}
    semantic_fingerprints = {item[3] for item in targets}
    if not targets:
        errors.append("AOT output has no program TargetPlan identity")
    elif module_count and target_indices != set(range(module_count)):
        errors.append(
            "TargetPlan module partitions do not cover the exact PSC module set"
        )
    if target_schemas and target_schemas != {schemas.target_schema}:
        errors.append(
            f"AOT TargetPlan schemas {sorted(target_schemas)} do not equal "
            f"{schemas.target_schema}"
        )
    if len(target_fingerprints) > 1:
        errors.append("AOT module partitions name more than one TargetPlan fingerprint")
    if len(semantic_fingerprints) > 1:
        errors.append("AOT module partitions name more than one SemanticPlan fingerprint")

    # This CLI path builds one verified in-memory TargetPlan but emits no XTP
    # artifact.  Record the absence instead of manufacturing an XTP identity
    # from the TargetPlan fingerprint.
    errors.append("AOT invocation emitted no exact verified XTP artifact identity")
    return PlanObservation(
        psc_schema=schemas.psc_schema,
        semantic_schema=schemas.semantic_schema,
        target_schema=next(iter(target_schemas), 0),
        xtp_schema=schemas.xtp_schema,
        module_count=module_count,
        dependency_count=dependency_count,
        program_fingerprint=program,
        module_set_fingerprint=module_set,
        generation_closure_id=generation,
        semantic_fingerprint=next(iter(semantic_fingerprints), ""),
        target_fingerprint=next(iter(target_fingerprints), ""),
        xtp_sha256="",
        xtp_verified=False,
        complete=False,
        errors=errors,
    )


def clip(raw: bytes) -> tuple[str, bool]:
    """Decode a captured stream and bound it to the reported prefix."""
    text = raw.decode("utf-8", errors="replace")
    if len(text) <= CAPTURE_CHARS:
        return text, False
    return text[:CAPTURE_CHARS], True


def run_command(
    argv: list[str], cwd: Path, timeout: float
) -> tuple[CommandResult, str, str]:
    """Return a bounded record plus untruncated stdout and stderr.

    The untruncated stderr is what diagnostic extraction reads: a refusal must
    be reported exactly as issued, and the reported prefix exists only to keep
    the record bounded.
    """
    try:
        completed = subprocess.run(
            argv,
            cwd=str(cwd),
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        out_raw, err_raw = completed.stdout, completed.stderr
        returncode: int | None = completed.returncode
        timed_out = False
    except subprocess.TimeoutExpired as expired:
        out_raw = expired.stdout or b""
        err_raw = expired.stderr or b""
        returncode = None
        timed_out = True
    stdout, stdout_truncated = clip(out_raw)
    stderr, stderr_truncated = clip(err_raw)
    return (
        CommandResult(
            argv=argv,
            returncode=returncode,
            timed_out=timed_out,
            stdout=stdout,
            stderr=stderr,
            stdout_truncated=stdout_truncated,
            stderr_truncated=stderr_truncated,
        ),
        out_raw.decode("utf-8", errors="replace"),
        err_raw.decode("utf-8", errors="replace"),
    )


def infer_stage(stderr: str, code: str) -> str:
    for marker, stage in STAGE_MARKERS:
        if marker in stderr:
            return stage
    for prefix, stage in CODE_PREFIX_STAGES:
        if code.startswith(prefix):
            return stage
    return "unknown"


def extract_first_refusal(stderr: str) -> FirstRefusal | None:
    """Return the opening refusal line of a failed run.

    A refusal is a line that carries a diagnostic code or an error marker.
    Warning lines are excluded because the compiler emits them alongside a
    failure without being the failure, and source echoes and carets carry
    neither marker.
    """
    for raw in stderr.splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        has_error = ERROR_MARKER_RE.search(line) is not None
        if not has_error and WARNING_MARKER_RE.search(line) is not None:
            continue
        match = DIAGNOSTIC_CODE_RE.search(line)
        if match is None and not has_error:
            continue
        code = match.group(0) if match else ""
        return FirstRefusal(diagnostic=line, code=code, stage=infer_stage(stderr, code))
    return None


def vm_argv(xray: Path, source: Path) -> list[str]:
    return [str(xray), "run", str(source)]


def aot_argv(xray: Path, source: Path, out_c: Path) -> list[str]:
    return [
        str(xray),
        "build",
        "--native",
        "--verbose",
        "--dump-xaot-plan",
        "-c",
        "-o",
        str(out_c),
        str(source),
    ]


def compare_regeneration(
    xray: Path, root: Path, source: Path, out_c: Path, timeout: float
) -> GeneratedC:
    """Regenerate C to the same path and compare the two results.

    Generated C embeds the absolute path of its source, and an output path can
    leak into a build the same way, so a second output path would report a
    difference that says nothing about determinism. Writing both generations to
    one path removes that variable; the first result is read into memory and the
    file removed before the second run, so a refused second run cannot be
    mistaken for a match against the file left behind.
    """
    if not out_c.is_file():
        return GeneratedC(
            checked=False,
            identical=None,
            first_sha256="",
            second_sha256="",
            first_bytes=None,
            second_bytes=None,
            second_returncode=None,
            note="first generation produced no C file",
        )
    first = out_c.read_bytes()
    out_c.unlink()
    second_result, _, _ = run_command(aot_argv(xray, source, out_c), root, timeout)
    if second_result.returncode != 0 or not out_c.is_file():
        return GeneratedC(
            checked=False,
            identical=None,
            first_sha256=hashlib.sha256(first).hexdigest(),
            second_sha256="",
            first_bytes=len(first),
            second_bytes=None,
            second_returncode=second_result.returncode,
            note="second generation did not produce C",
        )
    second = out_c.read_bytes()
    return GeneratedC(
        checked=True,
        identical=first == second,
        first_sha256=hashlib.sha256(first).hexdigest(),
        second_sha256=hashlib.sha256(second).hexdigest(),
        first_bytes=len(first),
        second_bytes=len(second),
        second_returncode=second_result.returncode,
        note="",
    )


def measure_module(
    module: str, root: Path, xray: Path, workspace: Path, timeout: float,
    schemas: SchemaAuthority,
) -> ModuleResult:
    probe = PROBE_ROOT / module / PROBE_RELATIVE
    absolute = root / probe
    if not absolute.is_file():
        return ModuleResult(
            module=module,
            probe=str(probe),
            probe_present=False,
            vm=None,
            aot=None,
            c_bytes=None,
            c_sha256="",
            generated_c=None,
            first_refusal=None,
            vm_plan=None,
            aot_plan=None,
        )

    # Each module gets its own directory because the AOT path drops an object
    # cache beside its output, and one shared directory would let modules read
    # each other's cached state.
    scratch = workspace / module
    scratch.mkdir(parents=True, exist_ok=True)
    out_c = scratch / f"{module}.c"

    vm_result, _, _ = run_command(vm_argv(xray, probe), root, timeout)
    aot_result, aot_stdout, aot_stderr = run_command(
        aot_argv(xray, probe, out_c), root, timeout
    )

    c_bytes: int | None = None
    c_sha256 = ""
    generated_c: GeneratedC | None = None
    first_refusal: FirstRefusal | None = None
    aot_plan: PlanObservation | None = None
    if aot_result.returncode == 0:
        aot_plan = parse_aot_plan_observation(aot_stdout, schemas)
        if out_c.is_file():
            payload = out_c.read_bytes()
            c_bytes = len(payload)
            c_sha256 = hashlib.sha256(payload).hexdigest()
        generated_c = compare_regeneration(xray, root, probe, out_c, timeout)
    else:
        first_refusal = extract_first_refusal(aot_stderr)

    return ModuleResult(
        module=module,
        probe=str(probe),
        probe_present=True,
        vm=vm_result,
        aot=aot_result,
        c_bytes=c_bytes,
        c_sha256=c_sha256,
        generated_c=generated_c,
        first_refusal=first_refusal,
        # The ordinary VM command currently reports no verified program-plan
        # identity.  Leaving this absent is what keeps the same-plan gate
        # UNRUN instead of inferring authority from a successful exit code.
        vm_plan=None,
        aot_plan=aot_plan,
    )


def measure_baseline(
    root: Path, xray: Path, workspace: Path, timeout: float,
    schemas: SchemaAuthority,
) -> tuple[bool, dict[str, Any]]:
    scratch = workspace / "_baseline"
    scratch.mkdir(parents=True, exist_ok=True)
    source = scratch / "baseline.xr"
    source.write_text(BASELINE_SOURCE, encoding="utf-8")
    out_c = scratch / "baseline.c"

    vm_result, _, _ = run_command(vm_argv(xray, source), root, timeout)
    aot_result, aot_stdout, aot_stderr = run_command(
        aot_argv(xray, source, out_c), root, timeout
    )
    refusal = None if aot_result.returncode == 0 else extract_first_refusal(aot_stderr)
    c_bytes = out_c.stat().st_size if out_c.is_file() else None
    payload = {
        "source": BASELINE_SOURCE,
        "vm": asdict(vm_result),
        "aot": asdict(aot_result),
        "c_bytes": c_bytes,
        "first_refusal": asdict(refusal) if refusal else None,
        "vm_plan": None,
        "aot_plan": (
            asdict(parse_aot_plan_observation(aot_stdout, schemas))
            if aot_result.returncode == 0
            else None
        ),
    }
    return aot_result.returncode == 0, payload


def cluster_refusals(results: list[ModuleResult]) -> dict[str, dict[str, Any]]:
    """Group failed AOT runs by the code of their opening refusal."""
    clusters: dict[str, list[str]] = {}
    for result in results:
        if result.first_refusal is None:
            continue
        key = result.first_refusal.code or "<no-code>"
        clusters.setdefault(key, []).append(result.module)
    ordered = sorted(clusters.items(), key=lambda item: (-len(item[1]), item[0]))
    return {
        code: {"count": len(modules), "modules": sorted(modules)} for code, modules in ordered
    }


def summarize(results: list[ModuleResult]) -> dict[str, Any]:
    measured = [r for r in results if r.probe_present]
    checked = [r for r in measured if r.generated_c and r.generated_c.checked]
    return {
        "modules": len(results),
        "probes_missing": sum(1 for r in results if not r.probe_present),
        "vm_ok": sum(1 for r in measured if r.vm and r.vm.returncode == 0),
        "vm_failed": sum(1 for r in measured if r.vm and r.vm.returncode != 0),
        "aot_ok": sum(1 for r in measured if r.aot and r.aot.returncode == 0),
        "aot_failed": sum(1 for r in measured if r.aot and r.aot.returncode != 0),
        "generated_c_compared": len(checked),
        "generated_c_identical": sum(1 for r in checked if r.generated_c and r.generated_c.identical),
        "generated_c_divergent": sum(
            1 for r in checked if r.generated_c and r.generated_c.identical is False
        ),
    }


def rc_cell(result: CommandResult | None) -> str:
    """Render an exit status, keeping a timeout distinct from any exit code."""
    if result is None:
        return "-"
    if result.timed_out:
        return "t/o"
    return str(result.returncode)


def redact_paths(payload: Any, workspace: Path, root: Path, xray: Path) -> Any:
    """Replace machine-local paths with stable report placeholders.

    The report is meant to be diffed between runs and committed, so a path that
    changes every run has to be removed from it. The replacement is textual
    because the name reaches the report inside recorded argv entries and inside
    captured compiler output alike.
    """
    if isinstance(payload, str):
        canonical = (
            (str(workspace), WORKSPACE_PLACEHOLDER),
            (str(xray), "<xray-binary>"),
            (str(root), "<source-root>"),
        )
        for marker, replacement in canonical:
            for spelling in {marker, marker.replace("\\", "/"), marker.replace("/", "\\")}:
                payload = payload.replace(spelling, replacement)
        return payload
    if isinstance(payload, list):
        return [redact_paths(item, workspace, root, xray) for item in payload]
    if isinstance(payload, dict):
        return {
            key: redact_paths(value, workspace, root, xray)
            for key, value in payload.items()
        }
    return payload


def render_table(results: list[ModuleResult], counts: dict[str, Any], baseline_ok: bool) -> str:
    lines: list[str] = []
    lines.append(f"baseline_ok: {'yes' if baseline_ok else 'NO'}")
    lines.append("")
    header = ("module", "vm rc", "aot rc", "c bytes", "first refusal", "stage")
    width = max(len(r.module) for r in results) if results else len(header[0])
    width = max(width, len(header[0]))
    lines.append(
        f"{header[0]:<{width}}  {header[1]:>5}  {header[2]:>6}  {header[3]:>8}  "
        f"{header[4]:<16}  {header[5]}"
    )
    lines.append(f"{'-' * width}  {'-' * 5}  {'-' * 6}  {'-' * 8}  {'-' * 16}  {'-' * 12}")
    for result in results:
        if not result.probe_present:
            lines.append(f"{result.module:<{width}}  {'-':>5}  {'-':>6}  {'-':>8}  probe missing")
            continue
        size = str(result.c_bytes) if result.c_bytes is not None else "-"
        if result.first_refusal is None:
            code, stage = "-", "-"
        else:
            code = result.first_refusal.code or "<no-code>"
            stage = result.first_refusal.stage
        lines.append(
            f"{result.module:<{width}}  {rc_cell(result.vm):>5}  {rc_cell(result.aot):>6}  "
            f"{size:>8}  {code:<16}  {stage}"
        )
    lines.append("")
    lines.append(
        f"vm ok {counts['vm_ok']}/{counts['modules']}, "
        f"aot ok {counts['aot_ok']}/{counts['modules']}, "
        f"generated C identical {counts['generated_c_identical']}/{counts['generated_c_compared']}"
    )
    return "\n".join(lines)


def source_identity(root: Path) -> tuple[dict[str, Any], list[str]]:
    """Return the exact Git source identity; a dirty tree is not evidence."""
    errors: list[str] = []

    def git(*args: str) -> str:
        completed = subprocess.run(
            ["git", *args], cwd=str(root), capture_output=True, text=True, check=False
        )
        if completed.returncode != 0:
            errors.append(
                f"git {' '.join(args)} failed: {completed.stderr.strip() or completed.stdout.strip()}"
            )
            return ""
        return completed.stdout.strip()

    commit = git("rev-parse", "HEAD")
    tree = git("rev-parse", "HEAD^{tree}")
    dirty = bool(git("status", "--porcelain", "--untracked-files=normal"))
    if dirty:
        errors.append("source tree has tracked modifications")
    return {"commit": commit, "tree": tree, "dirty": dirty}, errors


def compiler_identity(
    root: Path, xray: Path, source: dict[str, Any], timeout: float
) -> tuple[dict[str, Any], list[str]]:
    """Require the compiler binary to identify the exact source commit."""
    result, stdout, _ = run_command([str(xray), "--version", "--json"], root, timeout)
    errors: list[str] = []
    try:
        identity = json.loads(stdout)
    except json.JSONDecodeError as exc:
        identity = {}
        errors.append(f"compiler --version --json is invalid: {exc}")
    if result.returncode != 0:
        errors.append(f"compiler identity command returned {result.returncode}")
    if identity.get("commit") != source.get("commit"):
        errors.append(
            f"compiler commit {identity.get('commit')!r} != source commit "
            f"{source.get('commit')!r}"
        )
    if identity.get("dirty") is not False:
        errors.append("compiler identity is dirty or does not report dirty=false")
    features = set(identity.get("features") or ())
    if not {"vm", "aot"}.issubset(features):
        errors.append("compiler identity does not enable both vm and aot")
    return identity, errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--xray", default="", help="compiler binary (default <root>/build/xray)")
    parser.add_argument("--json", dest="json_path", help="write the machine-readable report")
    parser.add_argument("--jobs", type=int, default=4, help="modules measured in parallel")
    parser.add_argument(
        "--timeout", type=float, default=300.0, help="wall clock limit per invocation, seconds"
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    xray = Path(args.xray).resolve() if args.xray else root / "build/xray"
    if not xray.is_file():
        print(f"missing compiler binary: {xray}", file=sys.stderr)
        return 1
    if args.jobs < 1:
        print("--jobs must be at least 1", file=sys.stderr)
        return 1

    schemas, schema_errors = source_schema_authority(root)
    source, source_errors = source_identity(root)
    compiler, compiler_errors = compiler_identity(root, xray, source, args.timeout)
    authority_errors = [*schema_errors, *source_errors, *compiler_errors]
    if authority_errors:
        for error in authority_errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1

    modules = sorted(str(module["name"]) for module in load_manifest(root).modules)

    with tempfile.TemporaryDirectory(prefix="xray-backend-probe-") as tmp:
        workspace = Path(tmp)
        baseline_ok, baseline = measure_baseline(
            root, xray, workspace, args.timeout, schemas
        )
        collected: dict[str, ModuleResult] = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = {
                pool.submit(
                    measure_module, module, root, xray, workspace, args.timeout, schemas
                ): module
                for module in modules
            }
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                collected[result.module] = result
        # Workers finish in whatever order the toolchain allows; the report is
        # ordered by module name so two runs are comparable line by line.
        results = [collected[module] for module in modules]

    counts = summarize(results)
    payload = {
        "schema": SCHEMA,
        "authority": {
            "schemas": asdict(schemas),
            "schema_errors": schema_errors,
            "source": source,
            "compiler": compiler,
        },
        "baseline_ok": baseline_ok,
        "baseline": baseline,
        "counts": counts,
        "refusal_clusters": cluster_refusals(results),
        "modules": [asdict(result) for result in results],
    }
    # The scratch directory is named freshly on every run and its name reaches
    # the report through recorded command lines and compiler output. Left as
    # measured, two runs of the same tree would differ in the report and the
    # difference would carry no information, so the name is normalized out.
    payload = redact_paths(payload, workspace, root, xray)

    if args.json_path:
        Path(args.json_path).write_text(
            json.dumps(payload, indent=2, sort_keys=False) + "\n", encoding="utf-8"
        )

    print(render_table(results, counts, baseline_ok))
    if args.json_path:
        print(f"report: {args.json_path}")

    if not baseline_ok:
        print(
            "FAIL: the AOT path refused a program that imports nothing; "
            "module results describe the toolchain, not the standard library",
            file=sys.stderr,
        )
        return 1
    missing = [result.module for result in results if not result.probe_present]
    if missing:
        for module in missing:
            print(f"FAIL: manifest module has no contract probe: {module}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
