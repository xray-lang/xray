#!/usr/bin/env python3
"""Guard Xi patterned lowering from drifting back to handwritten dispatch."""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace


TARGET_TEMPLATE_MACROS = {
    "jit-xm": (
        "src/jit/xi_to_xm_dispatch_gen.h",
        {
            "XI_TO_XM_TEMPLATE_BINARY_DRIVERS",
            "XI_TO_XM_TEMPLATE_UNARY_DRIVERS",
            "XI_TO_XM_TEMPLATE_COMPARE_DRIVERS",
            "XI_TO_XM_TEMPLATE_WIDTH_DRIVERS",
        },
    ),
    "aot-c": (
        "src/aot/xi_to_c_dispatch_gen.h",
        {
            "XI_TO_C_TEMPLATE_ARITH_DRIVERS",
            "XI_TO_C_TEMPLATE_DIV_MOD_DRIVERS",
            "XI_TO_C_TEMPLATE_BITWISE_BINARY_DRIVERS",
            "XI_TO_C_TEMPLATE_BITWISE_UNARY_DRIVERS",
            "XI_TO_C_TEMPLATE_SHIFT_DRIVERS",
            "XI_TO_C_TEMPLATE_COMPARE_DRIVERS",
            "XI_TO_C_TEMPLATE_STRICT_COMPARE_DRIVERS",
            "XI_TO_C_TEMPLATE_WIDTH_DRIVERS",
        },
    ),
}

DIRECT_DRIVER_CHECKS = {
    "jit-xm": (
        "src/jit/xi_to_xm.c",
        r"\bstatic\s+XmRef\s+{driver}\s*\(",
        set(),
    ),
    "aot-c": (
        "src/aot/xi_cgen_dispatch_helpers.inc.c",
        r"\bstatic\s+void\s+{driver}\s*\(",
        {
            # Single semantic bodies, not duplicated per-op mapping wrappers.
            "xicgen_neg",
            "xicgen_not",
        },
    ),
}

CASE_SCAN_FILES = (
    "src/ir/xi_emit_arith.c",
    "src/jit/xi_to_xm.c",
    "src/aot/xi_cgen.c",
    "src/aot/xi_cgen_dispatch_helpers.inc.c",
)

VM_GENERATED_BODY_TEMPLATES = {"narrow", "widen"}
VM_GENERATED_WIDTH_FILE = "src/vm/xvm_template_width_gen.inc.c"
VM_BODY_SCAN_FILES = (
    "src/vm/xvm_dispatch_data.inc.c",
)


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    message: str


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def load_xisagen(root: Path):
    path = root / "tools/xisagen/xisagen.py"
    spec = importlib.util.spec_from_file_location("xisagen_impl", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def load_patterned_entries(root: Path):
    xisagen = load_xisagen(root)
    ops_path = root / "xisa/xi/ops.def"
    lowering_path = root / "xisa/xi/lowering.def"
    ops = xisagen.parse_xi_ops_def(ops_path.read_text(), str(ops_path))
    entries = xisagen.parse_xi_lowering_def(lowering_path.read_text(), ops, str(lowering_path))
    return [entry for entry in entries if entry.template != "custom"]


def vm_generated_body_opcodes(root: Path, entries) -> set[str]:
    xisagen = load_xisagen(root)
    opcodes: set[str] = set()
    for entry in entries:
        if 'vm-bytecode' in entry.target_drivers and entry.template in VM_GENERATED_BODY_TEMPLATES:
            opcodes.add(xisagen.XI_VM_TEMPLATE_OPCODES[entry.op_name])
    return opcodes


def parse_template_drivers(text: str, macro_names: set[str]) -> set[str]:
    drivers: set[str] = set()
    current_macro = None
    for line in text.splitlines():
        define = re.match(r"\s*#define\s+([A-Z0-9_]+)\s*\(X\)", line)
        if define:
            current_macro = define.group(1) if define.group(1) in macro_names else None
        elif current_macro is None:
            continue

        if current_macro is not None:
            match = re.search(r"\bX\s*\(\s*[A-Z0-9_]+\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", line)
            if match:
                drivers.add(match.group(1))
            if not line.rstrip().endswith("\\"):
                current_macro = None
    return drivers


def load_generated_template_drivers(root: Path) -> dict[str, set[str]]:
    generated: dict[str, set[str]] = {}
    for target, (rel, macros) in TARGET_TEMPLATE_MACROS.items():
        generated[target] = parse_template_drivers((root / rel).read_text(), macros)
    return generated


def gather_scan_files(root: Path) -> dict[str, str]:
    files: dict[str, str] = {}
    for rel in CASE_SCAN_FILES:
        for path in root.glob(rel):
            if path.is_file():
                files[path.relative_to(root).as_posix()] = path.read_text()
    return dict(sorted(files.items()))


def read_rel_files(root: Path, rels: tuple[str, ...]) -> dict[str, str]:
    files: dict[str, str] = {}
    for rel in rels:
        path = root / rel
        if path.is_file():
            files[rel] = path.read_text()
    return files


def check_generated_driver_coverage(entries, generated_by_target: dict[str, set[str]]) -> list[Violation]:
    violations: list[Violation] = []
    for entry in entries:
        for target, generated_drivers in generated_by_target.items():
            driver = entry.target_drivers.get(target)
            if driver is None:
                continue
            allowed_direct = DIRECT_DRIVER_CHECKS.get(target, ("", "", set()))[2]
            if driver not in generated_drivers and driver not in allowed_direct:
                violations.append(
                    Violation(
                        "xisa/xi/lowering.def",
                        0,
                        f"{entry.op_name} target {target} uses patterned driver {driver}, "
                        "but that driver is not emitted by a template driver macro",
                    )
                )
    return violations


def check_direct_driver_definitions(entries, texts: dict[str, str]) -> list[Violation]:
    violations: list[Violation] = []
    for target, (rel, pattern, allowed_direct) in DIRECT_DRIVER_CHECKS.items():
        text = texts.get(rel, "")
        for entry in entries:
            driver = entry.target_drivers.get(target)
            if driver is None or driver in allowed_direct:
                continue
            regex = re.compile(pattern.format(driver=re.escape(driver)))
            for line_no, line in enumerate(text.splitlines(), start=1):
                if regex.search(line):
                    violations.append(
                        Violation(
                            rel,
                            line_no,
                            f"{entry.op_name} target {target} driver {driver} is handwritten; "
                            "route it through the generated template driver macro",
                        )
                    )
    return violations


def check_patterned_cases(entries, texts: dict[str, str]) -> list[Violation]:
    idents = "|".join(re.escape(entry.ident) for entry in entries)
    case_re = re.compile(rf"\bcase\s+XI_({idents})\s*:")
    by_ident = {entry.ident: entry.op_name for entry in entries}
    violations: list[Violation] = []
    for rel, text in texts.items():
        for line_no, line in enumerate(text.splitlines(), start=1):
            match = case_re.search(line)
            if match:
                ident = match.group(1)
                violations.append(
                    Violation(
                        rel,
                        line_no,
                        f"{by_ident[ident]} has a handwritten concrete case; "
                        "use generated lowering metadata or a template macro",
                    )
                )
    return violations


def check_vm_generated_body_coverage(opcodes: set[str], generated_text: str) -> list[Violation]:
    emitted = set(
        re.findall(r"\bXVM_TEMPLATE_WIDTH_(?:INT|F32)_CASE\s*\(\s*(OP_[A-Z0-9_]+)",
                   generated_text)
    )
    missing = sorted(opcodes - emitted)
    extra = sorted(emitted - opcodes)
    violations: list[Violation] = []
    for opcode in missing:
        violations.append(
            Violation(
                VM_GENERATED_WIDTH_FILE,
                0,
                f"VM generated width body is missing {opcode}",
            )
        )
    for opcode in extra:
        violations.append(
            Violation(
                VM_GENERATED_WIDTH_FILE,
                0,
                f"VM generated width body contains undeclared template opcode {opcode}",
            )
        )
    return violations


def check_vm_handwritten_body_cases(opcodes: set[str], texts: dict[str, str]) -> list[Violation]:
    if not opcodes:
        return []
    opcode_re = "|".join(re.escape(opcode) for opcode in sorted(opcodes))
    vmcase_re = re.compile(rf"\bvmcase\s*\(\s*({opcode_re})\s*\)")
    violations: list[Violation] = []
    for rel, text in texts.items():
        for line_no, line in enumerate(text.splitlines(), start=1):
            match = vmcase_re.search(line)
            if match:
                violations.append(
                    Violation(
                        rel,
                        line_no,
                        f"{match.group(1)} has a handwritten VM handler; "
                        "route it through xvm_template_width_gen.inc.c",
                    )
                )
    return violations


def run_check(root: Path) -> list[Violation]:
    entries = load_patterned_entries(root)
    generated = load_generated_template_drivers(root)
    texts = gather_scan_files(root)
    vm_body_texts = read_rel_files(root, VM_BODY_SCAN_FILES)
    vm_generated_text = (root / VM_GENERATED_WIDTH_FILE).read_text()
    vm_body_opcodes = vm_generated_body_opcodes(root, entries)
    violations: list[Violation] = []
    violations.extend(check_generated_driver_coverage(entries, generated))
    violations.extend(check_direct_driver_definitions(entries, texts))
    violations.extend(check_patterned_cases(entries, texts))
    violations.extend(check_vm_generated_body_coverage(vm_body_opcodes, vm_generated_text))
    violations.extend(check_vm_handwritten_body_cases(vm_body_opcodes, vm_body_texts))
    return violations


def run_self_test() -> None:
    entries = [
        SimpleNamespace(
            op_name="xi.add",
            ident="ADD",
            target_drivers={"jit-xm": "xi2xm_add", "aot-c": "xicgen_add"},
        ),
        SimpleNamespace(
            op_name="xi.neg",
            ident="NEG",
            target_drivers={"aot-c": "xicgen_neg"},
        ),
    ]
    generated = {"jit-xm": {"xi2xm_add"}, "aot-c": {"xicgen_add"}}
    texts = {
        "src/jit/xi_to_xm.c": "static XmRef xi2xm_add(LowerCtx *ctx, XmBlock *blk, XiValue *v) {\n",
        "src/aot/xi_cgen_dispatch_helpers.inc.c": "static void xicgen_neg(XiCgenCtx *ctx) {\n",
        "src/ir/xi_emit_arith.c": "    case XI_ADD:\n",
    }
    direct = check_direct_driver_definitions(entries, texts)
    cases = check_patterned_cases(entries, texts)
    coverage = check_generated_driver_coverage(entries, generated)
    assert len(direct) == 1 and "xi.add" in direct[0].message
    assert len(cases) == 1 and "xi.add" in cases[0].message
    assert not coverage

    missing_generated = [
        SimpleNamespace(op_name="xi.sub", ident="SUB", target_drivers={"jit-xm": "xi2xm_sub"})
    ]
    missing = check_generated_driver_coverage(missing_generated, {"jit-xm": set(), "aot-c": set()})
    assert len(missing) == 1 and "xi.sub" in missing[0].message

    vm_opcodes = {"OP_NARROW_I8"}
    vm_coverage = check_vm_generated_body_coverage(
        vm_opcodes, "XVM_TEMPLATE_WIDTH_INT_CASE(OP_NARROW_I8, int8_t)\n")
    assert not vm_coverage
    vm_missing = check_vm_generated_body_coverage(vm_opcodes, "")
    assert len(vm_missing) == 1 and "OP_NARROW_I8" in vm_missing[0].message
    vm_handwritten = check_vm_handwritten_body_cases(
        vm_opcodes, {"src/vm/xvm_dispatch_data.inc.c": "vmcase(OP_NARROW_I8) {\n"})
    assert len(vm_handwritten) == 1 and "OP_NARROW_I8" in vm_handwritten[0].message


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="exercise failure detectors")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        print("check_xi_template_lowering: self-test passed")
        return 0

    violations = run_check(repo_root())
    if violations:
        print("check_xi_template_lowering: FAIL")
        for violation in violations:
            loc = violation.path if violation.line == 0 else f"{violation.path}:{violation.line}"
            print(f"  {loc}: {violation.message}")
        return 1

    entries = load_patterned_entries(repo_root())
    print(f"check_xi_template_lowering: OK ({len(entries)} patterned entries guarded)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
