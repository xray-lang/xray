#!/usr/bin/env python3
"""Named completion gates for an all-Xray standard library.

The legacy governance gate answers whether each module agrees with the policy
recorded for it in the boundary manifest. Agreement is not ownership: a module
whose recorded policy is `native_library` agrees with itself while every one of
its symbols is still owned by handwritten C. That gate can therefore be green
on a standard library whose meaning lives almost entirely outside Xray.

These gates answer the ownership question instead, one named gate per property
that has to hold before the standard library is written in Xray. Six of them
are derived from the per-symbol inventory, which reads source rather than
policy labels. Two of them need evidence a static reader cannot produce -- what
the backends actually accepted, and whether generated C rebuilds byte for byte
-- and report `UNRUN` when that evidence is absent, because an unobserved
property is not a satisfied one. The ninth aggregates the other eight.

Every gate that fails names the modules or symbols responsible, so a failure
states the remaining work rather than only its size.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stdlib_symbol_inventory import (  # noqa: E402
    QUEUE_ORDER,
    ModuleRow,
    SymbolRow,
    build_rows,
    is_semantic_c_owner,
    summarize,
)


SCHEMA = 1

PASS = "PASS"
FAIL = "FAIL"
UNRUN = "UNRUN"

# Any failing gate outranks any unrun gate: a known defect is a stronger
# result than a missing observation, and the caller has to be able to tell
# the two apart from the exit code alone.
EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_UNRUN = 2

# A failure lists enough offenders to start work on and states the full count,
# so the listing never reads as the whole population.
EVIDENCE_LIMIT = 20

LEGACY_GATE_NOTE = (
    "NOTE: scripts/report_stdlib_self_hosting.py --require-complete is a historical "
    "governance sub-gate over manifest policy agreement only; its passing does not mean "
    "the standard library is self-hosted in Xray."
)

NO_PROBE_EVIDENCE = "no backend probe evidence supplied"

UNRUN_WARNING = "not run; must not be counted as a pass"

UNRUN_BANNER = (
    "UNRUN gates were not run and must not be counted as passing: {names}"
)

# A native leaf is allowlisted only once it carries a class naming why the C
# boundary is permanent. The inventory records `unclassified` for every leaf
# because approving one needs a per-symbol record -- ABI, ownership, effect,
# provider, deletion trigger -- that the manifest schema cannot hold, so an
# empty or `unclassified` class is an unapproved leaf.
UNAPPROVED_LEAF_CLASSES = {"", "unclassified"}


# Probe field spellings. The probe writer and this reader are separate
# programs, so each fact is looked up under every name it plausibly carries
# and an unrecognised shape degrades to "no verdict" rather than to a crash.
TRUE_WORDS = {
    "pass", "passed", "ok", "true", "yes", "success", "succeeded", "green",
    "1", "complete", "reproducible", "identical", "match", "matched", "same",
    "unified", "accepted", "supported",
}
FALSE_WORDS = {
    "fail", "failed", "error", "no", "false", "red", "0", "refused", "refusal",
    "mismatch", "mismatched", "differ", "different", "unsupported", "rejected",
    "skipped", "missing",
}

MODULE_NAME_KEYS = ("module", "name", "module_name")
MODULE_LIST_KEYS = ("modules", "results", "probes", "entries", "records")
VM_KEYS = ("vm", "vm_ok", "vm_status", "vm_result", "vm_pass", "vm_passed")
AOT_KEYS = ("aot", "aot_ok", "aot_status", "aot_result", "aot_pass", "aot_passed")
STATUS_KEYS = ("ok", "status", "result", "passed", "pass", "success")
# A backend that records the invocation rather than a verdict states the
# outcome as a process result, where success is a zero return code.
RETURNCODE_KEYS = ("returncode", "return_code", "exit_code", "rc")
TIMEOUT_KEYS = ("timed_out", "timeout", "timed-out")
REFUSAL_KEYS = (
    "first_refusal", "first_refusal_message", "first_refusal_reason", "refusal",
    "refusal_message", "diagnostic", "reason", "message", "error",
)
REFUSAL_DETAIL_KEYS = ("code", "stage", "diagnostic")
HASH_LIST_KEYS = (
    "generated_c_sha256", "generated_c_sha256s", "generated_c_hashes",
    "c_sha256", "sha256", "sha256s", "hashes", "digests",
)
HASH_PAIR_KEYS = (
    ("generated_c_first_sha256", "generated_c_second_sha256"),
    ("sha256_first", "sha256_second"),
    ("first_sha256", "second_sha256"),
    ("first", "second"),
    ("a", "b"),
)
GENERATED_C_KEYS = ("generated_c", "generated_c_reproducibility", "codegen", "c_output")
REPRODUCIBLE_KEYS = (
    "generated_c_reproducible", "reproducible", "identical", "deterministic",
    "c_reproducible", "reproducibility", "stable",
)
PLAN_UNIFIED_KEYS = (
    "unified_plan", "plan_unified", "same_plan", "shared_plan",
    "unified_target_plan", "plan_identical",
)
VM_PLAN_KEYS = ("vm_plan_sha256", "vm_plan_id", "vm_plan", "vm_program_plan")
AOT_PLAN_KEYS = ("aot_plan_sha256", "aot_plan_id", "aot_plan", "aot_program_plan")
PLAN_KEYS = ("plan_sha256", "plan_id", "program_plan_sha256", "program_plan")


@dataclass
class GateResult:
    """One named gate: its verdict, its size and the offenders behind it."""

    name: str
    status: str
    message: str
    observed: int = 0
    expected: int = 0
    evidence: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def evidence_total(self) -> int:
        return len(self.evidence)


@dataclass
class ProbeModuleEvidence:
    """What the backend probe observed for one module."""

    name: str
    vm_ok: bool | None = None
    aot_ok: bool | None = None
    first_refusal: str = ""
    generated_c_hashes: list[str] = field(default_factory=list)
    generated_c_reproducible: bool | None = None
    vm_plan: str = ""
    aot_plan: str = ""
    plan_unified: bool | None = None

    def reproducibility_verdict(self) -> bool | None:
        """Report whether generated C rebuilt identically, if that is knowable.

        Two or more recorded digests settle the question by themselves; an
        explicit flag settles it when the probe reports the comparison instead
        of the digests. Anything else leaves the question open.
        """
        digests = [h for h in self.generated_c_hashes if h]
        if len(digests) >= 2:
            return len(set(digests)) == 1
        return self.generated_c_reproducible

    def plan_verdict(self) -> tuple[bool | None, str]:
        """Report whether both backends consumed one plan, with the reason.

        Recorded plan identities settle it directly. Otherwise a VM that
        accepts a module whose AOT lowering refuses proves the two backends did
        not consume the same accepted plan.
        """
        if self.plan_unified is not None:
            return self.plan_unified, (
                "probe reports the backends share one program plan"
                if self.plan_unified
                else "probe reports the backends do not share one program plan"
            )
        if self.vm_plan and self.aot_plan:
            same = self.vm_plan == self.aot_plan
            return same, (
                f"vm plan {self.vm_plan} == aot plan {self.aot_plan}"
                if same
                else f"vm plan {self.vm_plan} != aot plan {self.aot_plan}"
            )
        if self.vm_ok is None or self.aot_ok is None:
            return None, "probe records no vm/aot verdict"
        if self.vm_ok and not self.aot_ok:
            refusal = self.first_refusal or "no refusal recorded"
            return False, f"VM accepts the module, AOT refuses: {refusal}"
        if not self.vm_ok and not self.aot_ok:
            refusal = self.first_refusal or "no refusal recorded"
            return False, f"neither backend accepts the module: {refusal}"
        if not self.vm_ok and self.aot_ok:
            return False, "AOT accepts a module the VM refuses"
        return True, "both backends accept the module"


@dataclass
class ProbeEvidence:
    """The backend probe file, normalised, or the reason there is none."""

    available: bool = False
    source: str = ""
    error: str = NO_PROBE_EVIDENCE
    modules: dict[str, ProbeModuleEvidence] = field(default_factory=dict)


def pick(mapping: Any, keys: tuple[str, ...]) -> Any:
    """Return the first present, non-null value among the candidate keys."""
    if not isinstance(mapping, dict):
        return None
    for key in keys:
        if key in mapping and mapping[key] is not None:
            return mapping[key]
    return None


def as_bool(value: Any) -> bool | None:
    """Coerce a recorded verdict to a boolean, or to "no verdict"."""
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return bool(value)
    if isinstance(value, str):
        word = value.strip().lower()
        if word in TRUE_WORDS:
            return True
        if word in FALSE_WORDS:
            return False
        return None
    if isinstance(value, dict):
        return as_bool(pick(value, STATUS_KEYS))
    return None


def as_text(value: Any) -> str:
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return str(value)
    return ""


def refusal_text(value: Any) -> str:
    """Render a recorded refusal, whether it is a string or a structured record."""
    if isinstance(value, dict):
        parts = [as_text(value.get(key)) for key in REFUSAL_DETAIL_KEYS]
        parts = [part for part in parts if part]
        if parts:
            return " | ".join(parts)
        return as_text(pick(value, REFUSAL_KEYS))
    return as_text(value)


def backend_verdict(entry: dict[str, Any], keys: tuple[str, ...]) -> tuple[bool | None, str]:
    """Read one backend's verdict and any refusal it carries.

    A backend is recorded either as a scalar verdict or as a nested object. The
    nested form may state the outcome as a verdict or as the process result of
    the invocation, where a zero return code is the success and a timeout is a
    failure with no return code at all.
    """
    value = pick(entry, keys)
    if not isinstance(value, dict):
        return as_bool(value), ""
    refusal = refusal_text(pick(value, REFUSAL_KEYS))
    verdict = as_bool(pick(value, STATUS_KEYS))
    if verdict is None:
        if as_bool(pick(value, TIMEOUT_KEYS)) is True:
            verdict = False
        else:
            code = pick(value, RETURNCODE_KEYS)
            if isinstance(code, int) and not isinstance(code, bool):
                verdict = code == 0
    return verdict, refusal


def generated_c_records(entry: dict[str, Any]) -> tuple[list[dict[str, Any]], Any]:
    """Return the objects that may carry generated-C facts, most specific first.

    A dedicated generated-C record outranks the module record, whose own digest
    describes a single generation and therefore cannot settle reproducibility.
    """
    nested = pick(entry, GENERATED_C_KEYS)
    records: list[dict[str, Any]] = []
    if isinstance(nested, dict):
        records.append(nested)
    records.append(entry)
    return records, nested


def generated_c_hashes(entry: dict[str, Any]) -> list[str]:
    """Collect the digests recorded for a module's generated C."""
    records, nested = generated_c_records(entry)
    if isinstance(nested, list):
        return [as_text(item) for item in nested if as_text(item)]
    for record in records:
        for first_key, second_key in HASH_PAIR_KEYS:
            first = as_text(record.get(first_key))
            second = as_text(record.get(second_key))
            if first and second:
                return [first, second]
        value = pick(record, HASH_LIST_KEYS)
        if isinstance(value, list):
            digests = [as_text(item) for item in value if as_text(item)]
            if digests:
                return digests
        elif as_text(value):
            return [as_text(value)]
    return []


def generated_c_reproducible(entry: dict[str, Any]) -> bool | None:
    """Read the recorded reproducibility verdict, if the probe states one."""
    for record in generated_c_records(entry)[0]:
        verdict = as_bool(pick(record, REPRODUCIBLE_KEYS))
        if verdict is not None:
            return verdict
    return None


def probe_module_records(payload: Any) -> list[dict[str, Any]]:
    """Normalise the probe file's module records to a list of objects.

    The records are accepted as a bare list, as a list under one of several
    container keys, or as a name-keyed object.
    """
    container: Any = payload
    if isinstance(payload, dict):
        found = pick(payload, MODULE_LIST_KEYS)
        container = found if found is not None else payload
    records: list[dict[str, Any]] = []
    if isinstance(container, list):
        records = [item for item in container if isinstance(item, dict)]
    elif isinstance(container, dict):
        for key, value in container.items():
            if not isinstance(value, dict):
                continue
            entry = dict(value)
            if not as_text(pick(entry, MODULE_NAME_KEYS)):
                entry["module"] = key
            records.append(entry)
    return [r for r in records if as_text(pick(r, MODULE_NAME_KEYS))]


def load_probe_evidence(path_text: str) -> ProbeEvidence:
    """Read the backend probe file into the normalised evidence this file uses.

    Every field the gates read is resolved here, so a probe writer that spells
    a field differently is absorbed in one place. A file that cannot be read,
    parsed or recognised yields unavailable evidence, never an exception: the
    gates that depend on it then report `UNRUN`, which is the honest verdict
    for a property nothing observed.
    """
    if not path_text:
        return ProbeEvidence(error=NO_PROBE_EVIDENCE)
    path = Path(path_text)
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        return ProbeEvidence(source=str(path), error=f"cannot read probe evidence {path}: {error}")
    except (UnicodeError, json.JSONDecodeError) as error:
        return ProbeEvidence(source=str(path), error=f"probe evidence {path} is not readable JSON: {error}")

    records = probe_module_records(payload)
    if not records:
        return ProbeEvidence(
            source=str(path),
            error=f"probe evidence {path} carries no recognisable module records",
        )

    top_unified = as_bool(pick(payload, PLAN_UNIFIED_KEYS)) if isinstance(payload, dict) else None
    modules: dict[str, ProbeModuleEvidence] = {}
    for record in records:
        name = as_text(pick(record, MODULE_NAME_KEYS))
        vm_ok, vm_refusal = backend_verdict(record, VM_KEYS)
        aot_ok, aot_refusal = backend_verdict(record, AOT_KEYS)
        refusal = refusal_text(pick(record, REFUSAL_KEYS)) or aot_refusal or vm_refusal
        plan_unified = as_bool(pick(record, PLAN_UNIFIED_KEYS))
        shared_plan = as_text(pick(record, PLAN_KEYS))
        modules[name] = ProbeModuleEvidence(
            name=name,
            vm_ok=vm_ok,
            aot_ok=aot_ok,
            first_refusal=refusal,
            generated_c_hashes=generated_c_hashes(record),
            generated_c_reproducible=generated_c_reproducible(record),
            vm_plan=as_text(pick(record, VM_PLAN_KEYS)) or shared_plan,
            aot_plan=as_text(pick(record, AOT_PLAN_KEYS)) or shared_plan,
            plan_unified=plan_unified if plan_unified is not None else top_unified,
        )
    return ProbeEvidence(available=True, source=str(path), modules=modules)


def counted_gate(
    name: str,
    subject: str,
    offenders: list[str],
    notes: list[str] | None = None,
    singular: str = "",
) -> GateResult:
    """Build a gate whose property is that a counted population is empty.

    The failure line is read most often once the count is small, so a lone
    offender is named in the singular.
    """
    count = len(offenders)
    status = PASS if count == 0 else FAIL
    if count == 0:
        message = f"0 {subject}"
    elif count == 1:
        message = f"1 {singular or subject} remains (expected 0)"
    else:
        message = f"{count} {subject} remain (expected 0)"
    return GateResult(
        name=name,
        status=status,
        message=message,
        observed=count,
        expected=0,
        evidence=offenders,
        notes=list(notes or []),
    )


def drift_note(gate_subject: str, derived: int, counts: dict[str, Any], key: str) -> list[str]:
    """Report when this file's derivation disagrees with the inventory summary.

    The two are meant to state the same fact. A disagreement means one of them
    drifted, and hiding it would let a gate report a number no one can trace
    back to the inventory, so it is disclosed instead of reconciled.
    """
    summary = counts.get(key)
    if isinstance(summary, int) and summary != derived:
        return [
            f"inventory summary key {key}={summary} disagrees with the {derived} "
            f"{gate_subject} derived here; the stricter count is reported"
        ]
    return []


def gate_source_coverage(
    root: Path, modules: list[ModuleRow], rows: list[SymbolRow], counts: dict[str, Any]
) -> GateResult:
    """Every production module's meaning has to start from a real `.xr` file.

    A recorded `.xr` semantic source is only real when the file exists, holds
    content and is one of the module's own scanned sources; otherwise the
    manifest points at a source the module does not actually have.
    """
    offenders: list[str] = []
    notes: list[str] = []
    xray_owned: dict[str, int] = {}
    for row in rows:
        if row.xray_body:
            xray_owned[row.module] = xray_owned.get(row.module, 0) + 1

    for module in sorted(modules, key=lambda m: m.name):
        if module.audience != "production":
            continue
        source = module.semantic_source
        if not source.endswith(".xr"):
            offenders.append(
                f"{module.name}: semantic source {source or '<none>'} is not an .xr source "
                f"(policy {module.policy or '<none>'})"
            )
            continue
        path = root / source
        if not path.is_file():
            offenders.append(f"{module.name}: declared .xr semantic source {source} does not exist")
            continue
        if path.stat().st_size == 0:
            offenders.append(f"{module.name}: declared .xr semantic source {source} is empty")
            continue
        if source not in module.xr_sources:
            offenders.append(
                f"{module.name}: declared .xr semantic source {source} is not among the "
                f"module's scanned .xr sources"
            )
            continue
        if not xray_owned.get(module.name):
            notes.append(
                f"{module.name}: {source} exists but contributes no Xray-owned symbol row, "
                f"so the inventory cannot show what the file owns"
            )

    result = counted_gate(
        "stdlib_full_xray_source_coverage",
        "production modules without a real .xr semantic source",
        offenders,
        notes,
        singular="production module without a real .xr semantic source",
    )
    result.notes.extend(
        drift_note(
            "production modules without a real .xr semantic source",
            len(offenders),
            counts,
            "production_modules_without_xray_source",
        )
    )
    return result


def gate_no_whole_module_native_policy(
    modules: list[ModuleRow], counts: dict[str, Any]
) -> GateResult:
    """No production module may declare its whole body native.

    A whole-module native policy states that the module has no Xray body at
    all, which is the coarsest form of the defect the other gates count symbol
    by symbol.
    """
    offenders = [
        f"{m.name}: policy {m.policy} (layer {m.layer or '<none>'}, "
        f"semantic source {m.semantic_source or '<none>'})"
        for m in sorted(modules, key=lambda m: m.name)
        if m.audience == "production" and m.policy in {"native_primitive", "native_library"}
    ]
    result = counted_gate(
        "stdlib_no_whole_module_native_policy",
        "production modules declared native_primitive or native_library",
        offenders,
        singular="production module declared native_primitive or native_library",
    )
    result.notes.extend(
        drift_note(
            "whole-module native policies",
            len(offenders),
            counts,
            "whole_module_native_policy",
        )
    )
    return result


def gate_no_public_native_surface(
    modules: list[ModuleRow], counts: dict[str, Any]
) -> GateResult:
    """No public standard-library symbol may be a native binding.

    A public native symbol is C semantics exposed directly as the module's API,
    so it cannot be replaced without changing what callers depend on.
    """
    offenders: list[str] = []
    for module in sorted(modules, key=lambda m: m.name):
        if module.audience != "production":
            continue
        for symbol in module.public_native:
            offenders.append(f"{module.name}::{symbol} (policy {module.policy or '<none>'})")
    result = counted_gate(
        "stdlib_no_public_native_surface",
        "public native symbols in production modules",
        offenders,
        singular="public native symbol in a production module",
    )
    result.notes.extend(
        drift_note("public native symbols", len(offenders), counts, "public_native_symbols")
    )
    return result


def gate_native_leaf_allowlist(rows: list[SymbolRow], counts: dict[str, Any]) -> GateResult:
    """Every native leaf has to carry an approved allowlist class.

    A leaf is the one form of C the completion definition keeps, so each one
    needs a class stating why its boundary is permanent. Without that class the
    leaf is undecided residue, not an accepted boundary.
    """
    offenders = [
        f"{row.module}::{row.symbol} (kind {row.kind}, class "
        f"{row.leaf_class or '<none>'}"
        + (f"; {row.leaf_reason}" if row.leaf_reason else "")
        + ")"
        for row in sorted(rows, key=lambda r: (r.module, r.symbol))
        if row.native_leaf and row.leaf_class in UNAPPROVED_LEAF_CLASSES
    ]
    total_leaves = sum(1 for row in rows if row.native_leaf)
    notes = [
        f"{total_leaves} native leaves in total; an approved class needs a per-symbol "
        f"record naming ABI, ownership, effect, provider and deletion trigger"
    ]
    result = counted_gate(
        "stdlib_native_leaf_allowlist",
        "native leaves without an approved allowlist class",
        offenders,
        notes,
        singular="native leaf without an approved allowlist class",
    )
    result.notes.extend(
        drift_note(
            "unapproved native leaves",
            len(offenders),
            counts,
            "unclassified_native_leaf",
        )
    )
    return result


def gate_no_handwritten_c_semantic_owner(
    modules: list[ModuleRow], rows: list[SymbolRow], counts: dict[str, Any]
) -> GateResult:
    """No production symbol's meaning may be owned by handwritten C.

    This is the quantity the completion definition drives to zero. Per-module
    loaders and native leaves are excluded because they are residue of their
    own kinds, each with its own gate.
    """
    # Selection is by the row's own audience, matching the inventory summary.
    # A C owner that no manifest module claims is still a C owner, and being
    # ungoverned makes it worse rather than exempt, so it is counted here and
    # called out separately instead of being held to one side.
    manifest_modules = {m.name for m in modules}
    offending_rows = [
        row
        for row in sorted(rows, key=lambda r: (r.module, r.kind, r.symbol))
        if row.audience == "production" and is_semantic_c_owner(row)
    ]
    offenders = [
        f"{row.module}::{row.symbol} (kind {row.kind}, C owner "
        f"{row.handwritten_c_body or '<unknown>'})"
        for row in offending_rows
    ]
    orphans = [row for row in offending_rows if row.module not in manifest_modules]
    notes: list[str] = []
    if orphans:
        orphan_modules = sorted({row.module for row in orphans})
        notes.append(
            f"{len(orphans)} of the count above have no owning manifest module "
            f"({', '.join(orphan_modules)}), so no module policy governs them at all"
        )
    result = counted_gate(
        "stdlib_no_handwritten_c_semantic_owner",
        "handwritten C semantic owners",
        offenders,
        notes,
        singular="handwritten C semantic owner",
    )
    result.notes.extend(
        drift_note(
            "handwritten C semantic owners",
            len(offenders),
            counts,
            "handwritten_c_semantic_owner",
        )
    )
    return result


def gate_no_module_specific_c_loader(
    modules: list[ModuleRow], rows: list[SymbolRow], counts: dict[str, Any]
) -> GateResult:
    """No module may be loaded by a C factory written for that module.

    A per-module C loader keeps the module's registration in C even once its
    bodies are Xray, so a generic source-derived load path has to replace every
    one of them.
    """
    production = {m.name for m in modules if m.audience == "production"}
    loader_rows = [
        row
        for row in sorted(rows, key=lambda r: (r.module, r.symbol))
        if row.kind == "module-factory"
    ]
    offenders = [
        f"{row.module}::{row.symbol} ({row.factory_loader or row.semantic_source})"
        for row in loader_rows
    ]
    test_only = sorted({row.module for row in loader_rows if row.module not in production})
    notes: list[str] = []
    if test_only:
        notes.append(
            f"{len(loader_rows) - len(test_only)} of these loaders belong to production modules; "
            f"the remaining {len(test_only)} belong to non-production modules "
            f"({', '.join(test_only)}) and are counted here because the generic load path has to "
            f"replace them too"
        )
    result = counted_gate(
        "stdlib_no_module_specific_c_loader",
        "module-specific C loaders",
        offenders,
        notes,
        singular="module-specific C loader",
    )
    result.notes.extend(
        drift_note(
            "module-specific C loaders",
            len(offenders),
            counts,
            "module_specific_c_loaders",
        )
    )
    return result


def gate_generated_c_reproducibility(
    modules: list[ModuleRow], probe: ProbeEvidence
) -> GateResult:
    """Generated C has to rebuild identically from the same source and toolchain.

    Without that, the C checked in cannot be shown to be the source's output,
    and deleting a handwritten owner in favour of a generated one moves the
    trust rather than removing it. A static reader cannot observe this, so
    absent probe evidence the gate is unrun.
    """
    name = "stdlib_generated_c_reproducibility"
    if not probe.available:
        return GateResult(
            name=name,
            status=UNRUN,
            message=probe.error,
            notes=[UNRUN_WARNING],
        )

    production = [m.name for m in modules if m.audience == "production"]
    mismatched: list[str] = []
    uncovered: list[str] = []
    for module_name in production:
        record = probe.modules.get(module_name)
        verdict = record.reproducibility_verdict() if record else None
        if verdict is None:
            uncovered.append(module_name)
        elif not verdict:
            digests = record.generated_c_hashes if record else []
            detail = " != ".join(digests) if len(digests) >= 2 else "probe reports non-reproducible"
            mismatched.append(f"{module_name}: {detail}")

    if mismatched:
        result = counted_gate(
            name,
            "modules whose generated C did not rebuild identically",
            mismatched,
            singular="module whose generated C did not rebuild identically",
        )
        if uncovered:
            result.notes.append(
                f"{len(uncovered)} production "
                f"{'module carries' if len(uncovered) == 1 else 'modules carry'} no "
                f"reproducibility verdict: {', '.join(uncovered)}"
            )
        return result

    if uncovered:
        return GateResult(
            name=name,
            status=UNRUN,
            message=(
                f"probe evidence covers {len(production) - len(uncovered)} of {len(production)} "
                f"production modules; the rest carry no reproducibility verdict"
            ),
            observed=len(uncovered),
            evidence=[f"{module_name}: no reproducibility verdict" for module_name in uncovered],
            notes=[UNRUN_WARNING, f"probe evidence: {probe.source}"],
        )

    return GateResult(
        name=name,
        status=PASS,
        message=f"generated C rebuilt identically for all {len(production)} production modules",
        notes=[f"probe evidence: {probe.source}"],
    )


def gate_unified_target_plan_coverage(
    modules: list[ModuleRow], probe: ProbeEvidence
) -> GateResult:
    """The VM and AOT have to consume one verified program plan per module.

    Two backends that accept different programs mean the standard library has
    two meanings, and the one an Xray body is proven against is whichever
    backend ran. A VM that accepts a module its AOT lowering refuses is that
    split observed directly. A static reader cannot observe it, so absent probe
    evidence the gate is unrun.
    """
    name = "stdlib_unified_target_plan_coverage"
    if not probe.available:
        return GateResult(
            name=name,
            status=UNRUN,
            message=probe.error,
            notes=[UNRUN_WARNING],
        )

    production = [m.name for m in modules if m.audience == "production"]
    divergent: list[str] = []
    uncovered: list[str] = []
    for module_name in production:
        record = probe.modules.get(module_name)
        if record is None:
            uncovered.append(module_name)
            continue
        verdict, reason = record.plan_verdict()
        if verdict is None:
            uncovered.append(module_name)
        elif not verdict:
            divergent.append(f"{module_name}: {reason}")

    if divergent:
        result = counted_gate(
            name,
            "modules where the VM and AOT do not consume one verified plan",
            divergent,
            singular="module where the VM and AOT do not consume one verified plan",
        )
        if uncovered:
            result.notes.append(
                f"{len(uncovered)} production "
                f"{'module carries' if len(uncovered) == 1 else 'modules carry'} no "
                f"vm/aot verdict: {', '.join(uncovered)}"
            )
        return result

    if uncovered:
        return GateResult(
            name=name,
            status=UNRUN,
            message=(
                f"probe evidence covers {len(production) - len(uncovered)} of {len(production)} "
                f"production modules; the rest carry no vm/aot verdict"
            ),
            observed=len(uncovered),
            evidence=[f"{module_name}: no vm/aot verdict" for module_name in uncovered],
            notes=[UNRUN_WARNING, f"probe evidence: {probe.source}"],
        )

    return GateResult(
        name=name,
        status=PASS,
        message=f"VM and AOT consume one verified plan for all {len(production)} production modules",
        notes=[f"probe evidence: {probe.source}"],
    )


def gate_completion(components: list[GateResult]) -> GateResult:
    """Aggregate the component gates.

    A single failure fails the aggregate, and an unrun component leaves it
    unrun: completion is the conjunction of the properties, so an unobserved
    property cannot be counted toward it.
    """
    failed = [g for g in components if g.status == FAIL]
    unrun = [g for g in components if g.status == UNRUN]
    if failed:
        status = FAIL
    elif unrun:
        status = UNRUN
    else:
        status = PASS
    message = (
        f"{len(failed)} of {len(components)} component gates FAIL and {len(unrun)} are UNRUN "
        f"(expected {len(components)} PASS)"
        if status != PASS
        else f"all {len(components)} component gates pass"
    )
    return GateResult(
        name="stdlib_full_xray_self_hosting_complete",
        status=status,
        message=message,
        observed=len(failed) + len(unrun),
        expected=0,
        evidence=[f"{g.status} {g.name}: {g.message}" for g in components if g.status != PASS],
        notes=[UNRUN_WARNING] if status == UNRUN else [],
    )


def render(results: list[GateResult], verbose: bool) -> list[str]:
    lines: list[str] = []
    for result in results:
        lines.append(f"{result.status} {result.name}: {result.message}")
        if not verbose:
            continue
        shown = result.evidence[:EVIDENCE_LIMIT]
        for item in shown:
            lines.append(f"    - {item}")
        if result.evidence_total > len(shown):
            lines.append(
                f"    ... {result.evidence_total - len(shown)} more "
                f"({result.evidence_total} total)"
            )
        elif shown:
            lines.append(f"    ({result.evidence_total} total)")
        for note in result.notes:
            lines.append(f"    note: {note}")
    # The unrun warning is printed without --verbose as well: a reader who sees
    # only the gate lines has to be told that an unrun gate is not a pass.
    unrun = [r.name for r in results if r.status == UNRUN]
    if unrun:
        lines.append(UNRUN_BANNER.format(names=", ".join(unrun)))
    return lines


def exit_code(results: list[GateResult]) -> int:
    if any(r.status == FAIL for r in results):
        return EXIT_FAIL
    if any(r.status == UNRUN for r in results):
        return EXIT_UNRUN
    return EXIT_PASS


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument(
        "--probe-json",
        default="",
        help="backend probe evidence; without it the two evidence-backed gates report UNRUN",
    )
    parser.add_argument("--json", dest="json_path", help="write the machine-readable gate results")
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="list the modules and symbols behind each gate",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    modules, rows, _defects = build_rows(root)
    counts = summarize(modules, rows)
    probe = load_probe_evidence(args.probe_json)

    components = [
        gate_source_coverage(root, modules, rows, counts),
        gate_no_whole_module_native_policy(modules, counts),
        gate_no_public_native_surface(modules, counts),
        gate_native_leaf_allowlist(rows, counts),
        gate_no_handwritten_c_semantic_owner(modules, rows, counts),
        gate_no_module_specific_c_loader(modules, rows, counts),
        gate_generated_c_reproducibility(modules, probe),
        gate_unified_target_plan_coverage(modules, probe),
    ]
    results = components + [gate_completion(components)]
    code = exit_code(results)

    for line in render(results, args.verbose):
        print(line)
    print(LEGACY_GATE_NOTE)

    if args.json_path:
        payload = {
            "schema": SCHEMA,
            "root": str(root),
            "status": results[-1].status,
            "exit_code": code,
            "probe_evidence": {
                "supplied": bool(args.probe_json),
                "available": probe.available,
                "source": probe.source,
                "error": "" if probe.available else probe.error,
                "modules": len(probe.modules),
            },
            "counts": counts,
            "queue": {key: counts["queue"].get(key, 0) for key in QUEUE_ORDER},
            "gates": [
                {
                    "name": g.name,
                    "status": g.status,
                    "message": g.message,
                    "observed": g.observed,
                    "expected": g.expected,
                    "evidence_total": g.evidence_total,
                    "evidence": g.evidence,
                    "notes": g.notes,
                }
                for g in results
            ],
            "legacy_gate_note": LEGACY_GATE_NOTE,
        }
        path = Path(args.json_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    return code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
