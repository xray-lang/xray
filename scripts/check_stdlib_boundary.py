#!/usr/bin/env python3
"""Verify task-196 stdlib ownership, native boundary and dynamic-surface policy."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

from stdlib_manifest import (
    MANIFEST_PATH,
    VALID_LAYERS,
    VALID_POLICIES,
    api_inventory,
    def_public_symbols,
    dynamic_public_items,
    load_manifest,
    load_toml,
    registry_modules,
)

# The hosted-fragment ABI surface, as the generator emits it. Kept here rather
# than inline so a new batch or ownership mode is one edit and shows up in a
# grep, instead of a set literal buried in a loop.
HOSTED_ABI_BATCHES = {"scalar", "string_rc", "mutable_rc", "object_rc"}
HOSTED_ABI_OWNERSHIP = {"owned", "immediate", "owned-or-null", "immediate-or-null"}
# Results the VM passes by value; anything else is a heap handle.
HOSTED_ABI_SCALAR_RESULTS = {"()", "bool", "int", "float"}


def check_manifest(root: Path) -> list[str]:
    manifest = load_manifest(root)
    errors: list[str] = []
    if manifest.raw.get("schema") != 1:
        errors.append(f"{MANIFEST_PATH}: schema must be 1")
    if manifest.raw.get("governance", {}).get("export_authority") != (
        "boundary_manifest_semantic_source"
    ):
        errors.append(
            "governance.export_authority must be 'boundary_manifest_semantic_source'"
        )
    names = [str(module.get("name", "")) for module in manifest.modules]
    if len(names) != 34:
        errors.append(f"{MANIFEST_PATH}: task-256 terminal module count must be 34, got {len(names)}")
    if len(names) != len(set(names)):
        errors.append(f"{MANIFEST_PATH}: module names must be unique")
    source_registry = registry_modules(root)
    if set(names) != set(source_registry):
        missing = sorted(set(source_registry) - set(names))
        stale = sorted(set(names) - set(source_registry))
        if missing:
            errors.append(f"manifest misses registered modules: {', '.join(missing)}")
        if stale:
            errors.append(f"manifest lists unregistered modules: {', '.join(stale)}")

    ignored_dirs = {"defs", "types", "__pycache__"}
    source_dirs = {
        path.name
        for path in (root / "stdlib").iterdir()
        if path.is_dir() and path.name not in ignored_dirs and any(path.glob("*.c"))
    }
    untracked_dirs = sorted(source_dirs - set(names))
    if untracked_dirs:
        errors.append(f"stdlib native directories are not in manifest: {', '.join(untracked_dirs)}")

    for module in manifest.modules:
        name = str(module.get("name", ""))
        layer = module.get("layer")
        policy = module.get("policy")
        if layer not in VALID_LAYERS:
            errors.append(f"module {name}: invalid layer {layer!r}")
        if policy not in VALID_POLICIES:
            errors.append(f"module {name}: invalid policy {policy!r}")
        for field in ("semantic_source", "loader", "perf_suite"):
            if not module.get(field):
                errors.append(f"module {name}: missing {field}")
        for field in ("semantic_source", "loader"):
            value = module.get(field)
            if value and not (root / str(value)).is_file():
                errors.append(f"module {name}: {field} does not exist: {value}")
        expected_loader = source_registry.get(name)
        declared_loader = str(module.get("loader_symbol") or Path(str(module.get("loader", ""))).stem)
        if expected_loader and declared_loader != expected_loader:
            errors.append(
                f"module {name}: loader {declared_loader!r} does not match registry symbol "
                f"xr_load_module_{expected_loader}"
            )
    return errors


def check_builtin_distribution(root: Path) -> list[str]:
    """Keep the retained native-library modules inside the one stdlib boundary.

    ws left this set once its connection layer became pure Xray: it now has no
    core.def binding block and, like http, carries only a script loader.
    """
    errors: list[str] = []
    expected = {"cluster", "http2", "compress", "crypto"}
    manifest = load_manifest(root)
    names = set(manifest.by_name)
    core_def = (root / "stdlib/defs/core.def").read_text(encoding="utf-8")
    registry = (root / "src/module/xmodule.c").read_text(encoding="utf-8")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for name in sorted(expected):
        if name not in names:
            errors.append(f"built-in standard module {name}: missing boundary entry")
        module_dir = root / "stdlib" / name
        if not module_dir.is_dir():
            errors.append(f"built-in standard module {name}: missing stdlib/{name}")
        if not re.search(rf"^module\s+{re.escape(name)}\s*\{{", core_def, re.M):
            errors.append(f"built-in standard module {name}: binding block missing from core.def")
        if f'{{"{name}",' not in registry:
            errors.append(f"built-in standard module {name}: bare-name loader registration missing")
        entry = manifest.by_name.get(name, {})
        if entry.get("perf_suite") != f"stdlib/{name}":
            errors.append(f"built-in standard module {name}: perf_suite must be stdlib/{name}")

    forbidden = (
        "packages/official",
        "XR_PACKAGE_",
        "XR_OFFICIAL_PACKAGE",
        *(f"xray/{name}" for name in sorted(expected)),
    )
    checked = {
        "CMakeLists.txt": cmake,
        "src/module/xmodule.c": registry,
        "stdlib/defs/core.def": core_def,
    }
    for label, text in checked.items():
        for marker in forbidden:
            if marker in text:
                errors.append(f"{label}: removed official-package marker remains: {marker}")
    package_root = root / "packages" / "official"
    if package_root.exists() and any(path.is_file() for path in package_root.rglob("*")):
        errors.append("packages/official must contain no files after the atomic stdlib cutover")
    return errors


def check_builtin_schema(root: Path) -> list[str]:
    """Keep the public Iterator protocol declaration and compiler table aligned."""
    errors: list[str] = []
    declaration = root / "stdlib/types/iterator.xr"
    if not declaration.is_file():
        return ["missing built-in Iterator declaration: stdlib/types/iterator.xr"]
    text = declaration.read_text(encoding="utf-8")
    if not re.search(r"(?m)^class\s+Iterator<T>\s*\{", text):
        errors.append("stdlib/types/iterator.xr must declare class Iterator<T>")
    declared = set(re.findall(r"(?m)^\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", text))
    expected = {"hasNext", "next", "nth"}
    if declared != expected:
        errors.append(
            "Iterator declaration methods must be exactly hasNext/next/nth; got "
            + ", ".join(sorted(declared))
        )

    implementation = (root / "src/frontend/analyzer/xanalyzer_builtin_interfaces.c").read_text(
        encoding="utf-8"
    )
    table_match = re.search(
        r"static\s+XaInterfaceMethod\s+iterator_methods\[\]\s*=\s*\{(?P<body>.*?)\};",
        implementation,
        re.S,
    )
    implemented = set(re.findall(r'\{"([A-Za-z_][A-Za-z0-9_]*)"', table_match.group("body"))) if table_match else set()
    if implemented != expected:
        errors.append(
            "compiler Iterator method table must match stdlib/types/iterator.xr; got "
            + ", ".join(sorted(implemented))
        )

    items = api_inventory(root).get("items", [])
    iterator_items = {
        str(item.get("name", ""))
        for item in items
        if item.get("category") == "native-type" and item.get("namespace") == "Iterator"
    }
    if iterator_items != expected | {"Iterator"}:
        errors.append("API inventory does not expose the complete Iterator schema")

    embedded = (root / "src/frontend/analyzer/xnative_type_defs.inc.c").read_text(
        encoding="utf-8"
    )
    if 'X("iterator", xr_native_def_iterator)' not in embedded:
        errors.append("generated native type table does not embed iterator.xr")
    return errors


def check_l4_quality(root: Path) -> list[str]:
    """Fail closed on the task-256 algorithm and dynamic-value review."""
    manifest = load_manifest(root)
    errors: list[str] = []
    l4_modules = {
        str(module["name"])
        for module in manifest.modules
        if module.get("layer") == "L4" and module.get("policy") == "xray_semantic"
    }
    governance = manifest.raw.get("governance", {})
    if governance.get("l4_algorithm_review_task") != "task-256":
        errors.append("governance.l4_algorithm_review_task must be task-256")
    reviewed = set(governance.get("l4_algorithm_reviewed", ()))
    if reviewed != l4_modules:
        errors.append(
            "governance.l4_algorithm_reviewed must exactly match L4 xray_semantic modules"
        )

    strconv = (root / "stdlib/strconv/strconv.xr").read_text(encoding="utf-8")
    if ".runes().nth(" in strconv:
        errors.append("strconv parsing must remain a linear byte scan")
    if "return float(s)" not in strconv:
        errors.append("strconv.parseFloat must delegate conversion to the rounded binary64 primitive")
    if "multiplyLimit" not in strconv or "9223372036854775807" not in strconv:
        errors.append("strconv.parseInt must retain explicit signed-64-bit overflow guards")
    edge_case = (root / "tests/diff/cases/semantics/stdlib/strconv_contract_direct.xr").read_text(
        encoding="utf-8"
    )
    for needle in ("9223372036854775808", "1.00000000000000011102230246251565404236316680908203125", "1e+"):
        if needle not in edge_case:
            errors.append(f"strconv edge-case differential is missing {needle!r}")
    return errors


def check_l2_thinning(root: Path) -> list[str]:
    """Keep task-256's L2 cutover structural and measurable."""
    manifest = load_manifest(root)
    errors: list[str] = []
    expected_native = {
        "io": set(),
        "os": {"arch", "eol", "platform", "sep"},
        "net": {
            "NetConn", "NetConn.close", "NetConn.fd", "NetConn.isClosed", "NetConn.isTLS",
            "NetError", "NetListener", "NetListener.close", "NetListener.fd",
            "NetListener.isClosed", "NetListener.port",
        },
    }
    expected_sources = {
        "io": "stdlib/io/io.xr",
        "os": "stdlib/os/os.xr",
        "net": "stdlib/net/net.xr",
    }
    for name, native in expected_native.items():
        module = manifest.by_name.get(name, {})
        if module.get("policy") != "xray_semantic":
            errors.append(f"L2 module {name}: policy must be xray_semantic")
        if module.get("semantic_source") != expected_sources[name]:
            errors.append(f"L2 module {name}: semantic source must be {expected_sources[name]}")
        if set(module.get("public_native", ())) != native:
            errors.append(f"L2 module {name}: public_native is outside the terminal primitive/handle set")

    core_def = (root / "stdlib/defs/core.def").read_text(encoding="utf-8")
    for name in expected_native:
        block = re.search(
            rf"(?ms)^module\s+{re.escape(name)}\s*\{{(?P<body>.*?)(?=^module\s+|\Z)",
            core_def,
        )
        if not block:
            errors.append(f"L2 module {name}: missing core.def block")
            continue
        public_primitives = re.findall(r"(?m)^\s+fn\s+(?!__)([A-Za-z_][A-Za-z0-9_]*)\s*\{", block.group("body"))
        if public_primitives:
            errors.append(
                f"L2 module {name}: native functions must be private __* primitives: "
                + ", ".join(sorted(set(public_primitives)))
            )

    required_source_markers = {
        "io": ("class BufReader", "class BufWriter", "class LineIterator", "class FileStat"),
        "os": ("class ExecResult",),
        "net": ("class DialOptions", "class Endpoint", "class IpAddress", "fn lastError"),
    }
    for name, markers in required_source_markers.items():
        text = (root / expected_sources[name]).read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                errors.append(f"L2 module {name}: semantic source is missing {marker}")

    boundary_names = set(expected_native)
    xray_count = 0
    total_count = 0
    for item in api_inventory(root).get("items", []):
        if item.get("category") != "stdlib-module":
            continue
        module = str(item.get("doc_module") or item.get("namespace") or "")
        if module not in boundary_names:
            continue
        total_count += 1
        if str(item.get("source", "")).endswith(".xr"):
            xray_count += 1
    ratio = (xray_count / total_count) if total_count else 0.0
    if ratio < 0.85:
        errors.append(
            f"task-256 Xray public-symbol ownership ratio is {ratio:.4%}; expected >= 85%"
        )

    governance = manifest.raw.get("governance", {})
    triggers = set(governance.get("regex_reassessment_triggers", ()))
    expected_triggers = {
        "generated_stdlib_vm_fastpaths_fully_landed",
        "freestanding_runtime_requires_regex_without_native_library",
    }
    if triggers != expected_triggers:
        errors.append("regex reassessment triggers must match the task-256 terminal decision")

    if re.search(r"\bos\.chdir\b|\bxrt_os_chdir\b", core_def + (root / "src/aot/xrt_os.h").read_text(encoding="utf-8")):
        errors.append("duplicate os.chdir boundary must not return; io.chdir is the sole owner")
    return errors


def check_semantic_owners(root: Path) -> list[str]:
    manifest = load_manifest(root)
    errors: list[str] = []
    def_symbols = def_public_symbols(root)
    for module in manifest.modules:
        name = str(module["name"])
        source = root / str(module["semantic_source"])
        if module["policy"] == "xray_semantic":
            if source.suffix != ".xr":
                errors.append(f"module {name}: xray_semantic owner must be an .xr source")
            elif not re.search(
                r"(?m)^export\s+(?:(?:final|packed)\s+)?"
                r"(?:fn|class|struct|union|interface|enum|type|const|shared)\b",
                source.read_text(encoding="utf-8"),
            ):
                errors.append(
                    f"module {name}: xray_semantic source has no directly exported declaration"
                )
        declared = set(module.get("public_native", ()))
        manual = set(module.get("manual_public_native", ()))
        actual = def_symbols.get(name, set()) | manual
        if declared != actual:
            missing = sorted(actual - declared)
            stale = sorted(declared - actual)
            if missing:
                errors.append(f"module {name}: public_native misses .def symbols: {', '.join(missing)}")
            if stale:
                errors.append(f"module {name}: public_native has stale symbols: {', '.join(stale)}")
        if manual:
            loader_text = (root / str(module["loader"])).read_text(encoding="utf-8")
            for symbol in sorted(manual):
                leaf = symbol.rsplit(".", 1)[-1]
                if f'"{leaf}"' not in loader_text:
                    errors.append(
                        f"module {name}: manual public native {symbol!r} is not registered by loader"
                    )
        private_sources = module.get("private_native_sources", ())
        if private_sources and not module.get("private_native_reason"):
            errors.append(f"module {name}: private native sources require private_native_reason")
        for pattern in private_sources:
            if not list(root.glob(str(pattern))):
                errors.append(f"module {name}: private native source pattern matches nothing: {pattern}")
        runtime_adapters = module.get("aot_runtime_adapters", ())
        if runtime_adapters and not module.get("aot_runtime_adapter_reason"):
            errors.append(f"module {name}: AOT runtime adapters require a reason")
        if runtime_adapters and not module.get("aot_helper_forbidden"):
            errors.append(
                f"module {name}: AOT runtime adapters are only valid with module helper residue forbidden"
            )
        expected_prefix = f"xrt_{name}_"
        for symbol in runtime_adapters:
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str(symbol)):
                errors.append(f"module {name}: invalid AOT runtime adapter symbol {symbol!r}")
            elif not str(symbol).startswith(expected_prefix):
                errors.append(
                    f"module {name}: AOT runtime adapter {symbol!r} must use {expected_prefix!r}"
                )
    return errors


def check_error_model_policy(root: Path) -> list[str]:
    """Keep the superseded global Result track out of the prelude.

    User and module-defined enums named Result remain ordinary legal ADTs. This
    gate is intentionally limited to compiler/prelude registration surfaces.
    """
    manifest = load_manifest(root)
    governance = manifest.raw.get("governance", {})
    errors: list[str] = []
    if governance.get("error_model") != "enum_value_channel":
        errors.append("governance.error_model must be 'enum_value_channel'")
    if governance.get("failure_as_data") != "domain_adt":
        errors.append("governance.failure_as_data must be 'domain_adt'")

    forbidden = governance.get("forbidden_prelude_symbols", ())
    if not isinstance(forbidden, list) or not forbidden:
        errors.append("governance.forbidden_prelude_symbols must be a non-empty list")
        return errors

    high_risk_sources = (
        root / "src/base/xglobal_indices.h",
        root / "stdlib/prelude/prelude.c",
        root / "src/frontend/analyzer/xanalyzer.c",
        root / "src/api/xisolate_runtime.c",
        root / "src/coro/xaot_coro.c",
        root / "scripts/gen_api_inventory.py",
    )
    for raw_symbol in forbidden:
        symbol = str(raw_symbol)
        type_decl = root / "stdlib/types" / f"{symbol.lower()}.xr"
        if type_decl.exists():
            errors.append(f"forbidden prelude symbol {symbol}: declaration exists: {type_decl}")

        macro = "XR_GLOBAL_VAR_" + re.sub(r"[^A-Za-z0-9]", "_", symbol).upper()
        patterns = (
            re.compile(rf"\b{re.escape(macro)}\b"),
            re.compile(rf"\b(?:register|make)_prelude_enum(?:_full)?\s*\([^;]*\"{re.escape(symbol)}\"", re.S),
            re.compile(rf"\bPRELUDE_ENUMS\b[^;]*\(\s*\"{re.escape(symbol)}\"", re.S),
        )
        for source in high_risk_sources:
            text = source.read_text(encoding="utf-8")
            if any(pattern.search(text) for pattern in patterns):
                errors.append(f"forbidden prelude symbol {symbol}: registered by {source}")
    return errors


def check_fastpaths(root: Path) -> list[str]:
    manifest = load_manifest(root)
    errors: list[str] = []
    object_abi = manifest.raw.get("object_abi", {})
    expected_object_abi = {
        "version": 1,
        "value_layout": "include/xray_value_abi.h",
        "object_header": "src/shared/xr_obj_header.h",
        "entry_abi": "XrValue(XrHostedFragmentContext*,XrValue*,uint32_t;signal)",
        "error_channel": "vm_pending_error",
        "ownership": "borrow_args_owned_result",
        "unsupported_policy": "fail_closed",
    }
    for field, expected in expected_object_abi.items():
        if object_abi.get(field) != expected:
            errors.append(f"object_abi.{field} must be {expected!r}")
    canonical_value = root / "include/xray_value_abi.h"
    if not canonical_value.is_file():
        errors.append("hosted object ABI canonical XrValue header is missing")
    else:
        value_text = canonical_value.read_text(encoding="utf-8")
        if value_text.count("typedef struct XrValue") != 1:
            errors.append("include/xray_value_abi.h must contain the sole hosted XrValue typedef")
        if "XR_HOSTED_OBJECT_ABI_VERSION" not in value_text:
            errors.append("canonical XrValue header must expose XR_HOSTED_OBJECT_ABI_VERSION")
    hosted_abi = root / "include/xray_hosted_fragment_abi.h"
    if not hosted_abi.is_file():
        errors.append("canonical hosted-fragment call ABI header is missing")
    else:
        hosted_text = hosted_abi.read_text(encoding="utf-8")
        for required_symbol in (
            "XR_HOSTED_FRAGMENT_ABI_VERSION",
            "XrHostedFragmentContext",
            "XrHostedFragmentSignal",
            "XrHostedFragmentEntry",
        ):
            if required_symbol not in hosted_text:
                errors.append(f"hosted-fragment ABI is missing {required_symbol}")
    value_consumers = (
        "include/xray_runtime.h",
        "src/runtime/value/xvalue.h",
        "src/aot/xrt_value.h",
        "src/aot/xrt_core_freestanding.h",
    )
    for relative in value_consumers:
        text = (root / relative).read_text(encoding="utf-8")
        if "xray_value_abi.h" not in text:
            errors.append(f"{relative}: must consume canonical xray_value_abi.h")
        if "typedef struct XrValue" in text:
            errors.append(f"{relative}: duplicates the canonical hosted XrValue layout")
    generator = root / "tools/stdlibgen/generate_vm_fastpaths.py"
    if not generator.is_file():
        errors.append("stdlib generated-native source generator is missing")
        generated_entries: list[dict[str, object]] = []
    else:
        try:
            spec = importlib.util.spec_from_file_location("xray_vm_fragment_generator", generator)
            if not spec or not spec.loader:
                raise RuntimeError("cannot create generator module spec")
            generator_module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = generator_module
            spec.loader.exec_module(generator_module)
            # load_entries returns five values; unpacking fewer raised inside the
            # fail-closed handler below, which reported the drift as "cannot derive
            # source-backed VM fragment entries" and then, with an empty entry list,
            # as a bogus benchmark-baseline mismatch. Keep every field named so the
            # next signature change is a syntax-visible edit rather than a runtime one.
            (_version, generated_entries, _deferred, _unsupported,
             _fingerprint) = generator_module.load_entries(root)
            if len(generated_entries) < 48:
                errors.append("source-derived hosted fragment batches must contain at least 48 exports")
            # These assert the shape of the ABI, not the generator's type
            # classification. Re-deriving "which results are heap values" here
            # is what went stale: the gate only knew string and Array<byte>,
            # so every class-instance and enum result read as a violation once
            # object_rc arrived. Owning the invariants, and only the
            # invariants, keeps the gate honest without a second copy of the
            # type rules that has to be updated in lockstep.
            for generated in generated_entries:
                symbol = str(generated.get("symbol", ""))
                if generated.get("batch") not in HOSTED_ABI_BATCHES:
                    errors.append(f"generated fragment {symbol}: unsupported hosted ABI batch")
                if generated.get("effect") != "compiler-verified":
                    errors.append(f"generated fragment {symbol}: missing compiler effect provenance")

                result = str(generated.get("result", ""))
                ownership = str(generated.get("ownership", ""))
                if ownership not in HOSTED_ABI_OWNERSHIP:
                    errors.append(f"generated fragment {symbol}: unknown result ownership")
                    continue
                # A nullable result must say so, and a non-nullable one must not:
                # the suffix is what tells the VM whether null is a legal value.
                if result.endswith("?") != ownership.endswith("-or-null"):
                    errors.append(
                        f"generated fragment {symbol}: ownership nullability "
                        "disagrees with result type")
                base = ownership.split("-or-null")[0]
                bare = result[:-1] if result.endswith("?") else result
                # The original ABI rule, still the one that matters: a result
                # the callee allocates must be handed over as owned, or the VM
                # leaks it or double-frees it.
                if (bare == "string" or bare.startswith("Array<")) and base != "owned":
                    errors.append(
                        f"generated fragment {symbol}: heap result must be owned")
                if bare in HOSTED_ABI_SCALAR_RESULTS and base != "immediate":
                    errors.append(
                        f"generated fragment {symbol}: scalar result must be immediate")
            harness, _project = generator_module.render_harness(generated_entries, _fingerprint)
            if "import http" not in harness or "return http.isRedirectStatus(" not in harness:
                errors.append("generated fragment harness must call authoritative imported .xr exports")
            if re.search(r"export\s+fn\s+isRedirectStatus\s*\(", harness):
                errors.append("generated fragment harness must not copy authoritative function bodies")
        except Exception as exc:  # fail closed: generator/import inventory is part of the contract
            generated_entries = []
            errors.append(f"cannot derive source-backed VM fragment entries: {exc}")
    option_name = str(manifest.raw.get("governance", {}).get("vm_fastpath_option", ""))
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    if not option_name or not re.search(rf"option\(\s*{re.escape(option_name)}\b", cmake):
        errors.append("governance.vm_fastpath_option must name a real CMake option")
    required = {
        "symbol", "module", "member", "reference", "native", "abi", "batch", "reason",
        "abi_gate", "workload_benchmark", "diff_case", "oracle", "review_date",
        "disable_build_option"
    }
    benchmark_data = load_toml(root / "tests/benchmarks/stdlib/manifest.toml")
    benchmark_ids = {entry.get("id") for entry in benchmark_data.get("benchmark", ())}
    unit_cmake = (root / "tests/unit/CMakeLists.txt").read_text(encoding="utf-8")
    aot_text = "\n".join(
        path.read_text(encoding="utf-8", errors="strict")
        for path in (root / "src/aot").rglob("*")
        if path.is_file() and path.suffix in {".c", ".h"}
    )
    seen: set[str] = set()
    for entry in manifest.vm_fastpaths:
        symbol = str(entry.get("symbol", ""))
        if symbol in seen:
            errors.append(f"duplicate vm_fastpath symbol: {symbol}")
        seen.add(symbol)
        missing = sorted(required - set(entry))
        if missing:
            errors.append(f"vm_fastpath {symbol or '<unnamed>'}: missing {', '.join(missing)}")
        module = str(entry.get("module", ""))
        member = str(entry.get("member", ""))
        if symbol != f"{module}.{member}":
            errors.append(f"vm_fastpath {symbol}: symbol must equal module.member")
        if entry.get("abi") != "i64->bool" or entry.get("batch") != "scalar":
            errors.append(f"vm_fastpath {symbol}: first generated batch only admits i64->bool scalar ABI")
        reference_raw = str(entry.get("reference", ""))
        reference_path, separator, reference_member = reference_raw.partition("::")
        if reference_path and not (root / reference_path).is_file():
            errors.append(f"vm_fastpath {symbol}: reference source does not exist: {reference_path}")
        elif reference_path:
            reference_text = (root / reference_path).read_text(encoding="utf-8")
            if not separator or reference_member != member or not re.search(
                rf"\bexport\s+fn\s+{re.escape(member)}\s*\(", reference_text
            ):
                errors.append(f"vm_fastpath {symbol}: reference must name its exported .xr function")
        oracle_path = str(entry.get("oracle", "")).split("::", 1)[0]
        if not oracle_path or not (root / oracle_path).is_file():
            errors.append(f"vm_fastpath {symbol}: migration C oracle does not exist: {oracle_path}")
        if entry.get("disable_build_option") != option_name:
            errors.append(f"vm_fastpath {symbol}: must use global disable option {option_name}")
        abi_gate = str(entry.get("abi_gate", ""))
        if not abi_gate or not re.search(rf"\b{re.escape(abi_gate)}\b", unit_cmake):
            errors.append(f"vm_fastpath {symbol}: abi_gate is absent from unit CMake targets")
        if entry.get("workload_benchmark") not in benchmark_ids:
            errors.append(
                f"vm_fastpath {symbol}: workload_benchmark id is absent from stdlib perf manifest"
            )
        diff_case = root / str(entry.get("diff_case", ""))
        if not diff_case.is_file():
            errors.append(f"vm_fastpath {symbol}: diff_case does not exist: {entry.get('diff_case')}")
        native = str(entry.get("native", ""))
        if native and re.search(rf"\b{re.escape(native)}\b", aot_text):
            errors.append(f"vm_fastpath {symbol}: AOT sources reference VM-only symbol {native}")
    generated_symbols = {str(entry.get("symbol", "")) for entry in generated_entries}
    if not seen.issubset(generated_symbols):
        errors.append(
            "VM fragment benchmark baselines are not a subset of the source-derived scalar batch: "
            + ", ".join(sorted(seen - generated_symbols))
        )
    return errors


def _dynamic_module(item: dict[str, object]) -> str:
    return str(item.get("doc_module") or item.get("namespace") or "")


def check_dynamic(root: Path, require_clean: bool = False) -> tuple[list[str], dict[str, object]]:
    manifest = load_manifest(root)
    policy = manifest.raw.get("dynamic_audit", {})
    migration_modules = set(policy.get("migration_modules", ()))
    allowlist = tuple(manifest.raw.get("dynamic_allowlist", ()))
    allowed: set[str] = set()
    items = dynamic_public_items(root)
    errors: list[str] = []
    debt: list[dict[str, object]] = []
    required = {"symbol", "direction", "domain", "reason", "owner", "review_task"}
    valid_directions = {"input", "output", "field"}
    valid_domains = {"json_document", "json_wire_payload", "explicit_bridge"}
    for index, entry in enumerate(allowlist, 1):
        missing = sorted(required - set(entry))
        symbol = str(entry.get("symbol", ""))
        label = symbol or f"entry {index}"
        if missing:
            errors.append(f"dynamic_allowlist {label}: missing {', '.join(missing)}")
        if not symbol:
            continue
        if symbol in allowed:
            errors.append(f"dynamic_allowlist contains duplicate symbol: {symbol}")
        allowed.add(symbol)
        if entry.get("direction") not in valid_directions:
            errors.append(f"dynamic_allowlist {symbol}: invalid direction {entry.get('direction')!r}")
        if entry.get("domain") not in valid_domains:
            errors.append(f"dynamic_allowlist {symbol}: invalid domain {entry.get('domain')!r}")
        if not str(entry.get("reason", "")).strip():
            errors.append(f"dynamic_allowlist {symbol}: reason must be non-empty")
        if not str(entry.get("owner", "")).strip():
            errors.append(f"dynamic_allowlist {symbol}: owner must be non-empty")
        if not re.fullmatch(r"task-[0-9]+", str(entry.get("review_task", ""))):
            errors.append(f"dynamic_allowlist {symbol}: review_task must be task-<number>")
    for item in items:
        symbol = str(item.get("qualified", ""))
        module = _dynamic_module(item)
        if symbol in allowed:
            continue
        if module in migration_modules:
            debt.append(
                {
                    "module": module,
                    "symbol": symbol,
                    "signature": item.get("signature", ""),
                    "source": item.get("source", ""),
                    "line": item.get("line", 0),
                }
            )
            continue
        errors.append(
            f"unclassified public Json/unknown surface: {module}:{symbol} "
            f"{item.get('signature', '')} ({item.get('source', '')}:{item.get('line', 0)})"
        )
    actual_symbols = {str(item.get("qualified", "")) for item in items}
    for symbol in sorted(allowed - actual_symbols):
        errors.append(f"dynamic_audit.allowed_symbols contains stale symbol: {symbol}")
    if require_clean and debt:
        errors.append(f"dynamic migration debt remains: {len(debt)} public surfaces")
    report = {
        "schema": 1,
        "allowed_count": sum(1 for item in items if str(item.get("qualified", "")) in allowed),
        "allowlist": list(allowlist),
        "migration_debt_count": len(debt),
        "migration_debt": debt,
    }
    return errors, report


CHECKS = {
    "manifest", "semantic", "l2-thinning", "builtin-schema", "l4-quality", "error-model", "fastpath",
    "dynamic", "all"
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--check", choices=sorted(CHECKS), default="all")
    parser.add_argument("--report-json", type=Path)
    parser.add_argument("--require-clean-dynamic", action="store_true")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    errors: list[str] = []
    report: dict[str, object] = {}
    if args.check in {"manifest", "all"}:
        errors.extend(check_manifest(root))
        errors.extend(check_builtin_distribution(root))
    if args.check in {"semantic", "all"}:
        errors.extend(check_semantic_owners(root))
    if args.check in {"l2-thinning", "all"}:
        errors.extend(check_l2_thinning(root))
    if args.check in {"builtin-schema", "all"}:
        errors.extend(check_builtin_schema(root))
    if args.check in {"l4-quality", "all"}:
        errors.extend(check_l4_quality(root))
    if args.check in {"error-model", "all"}:
        errors.extend(check_error_model_policy(root))
    if args.check in {"fastpath", "all"}:
        errors.extend(check_fastpaths(root))
    if args.check in {"dynamic", "all"}:
        dynamic_errors, report = check_dynamic(root, args.require_clean_dynamic)
        errors.extend(dynamic_errors)
    if args.report_json:
        args.report_json.parent.mkdir(parents=True, exist_ok=True)
        args.report_json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if errors:
        print("stdlib boundary gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    debt_count = int(report.get("migration_debt_count", 0))
    suffix = f"; {debt_count} classified dynamic migration debts" if report else ""
    print(f"OK: stdlib boundary {args.check} governance is source-consistent{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
