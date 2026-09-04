#!/usr/bin/env python3
"""Focused unit coverage for the task-196 stdlib boundary manifest."""

from __future__ import annotations

import copy
import contextlib
import dataclasses
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from check_stdlib_boundary import (  # noqa: E402
    check_dynamic,
    check_error_model_policy,
    check_fastpaths,
    check_manifest,
    check_semantic_owners,
)
from stdlib_manifest import (  # noqa: E402
    BoundaryManifest,
    load_manifest,
    loadable_modules,
    load_stdlibgen,
    native_entry_binder_modules,
    source_modules,
)
from report_stdlib_self_hosting import (  # noqa: E402
    benchmark_backend_agreement,
    benchmark_oracle_agreement,
    build_report,
)
from check_stdlib_aot_helper_residue import check_forbidden_tokens  # noqa: E402
from check_binary_stdlib_runtime_baseline import check_runner_source  # noqa: E402
import stdlib_migration  # noqa: E402


_BENCHMARK_RUNNER_SPEC = importlib.util.spec_from_file_location(
    "stdlib_benchmark_runner_under_test", ROOT / "tests/benchmarks/stdlib/run.py"
)
assert _BENCHMARK_RUNNER_SPEC is not None and _BENCHMARK_RUNNER_SPEC.loader is not None
benchmark_runner = importlib.util.module_from_spec(_BENCHMARK_RUNNER_SPEC)
_BENCHMARK_RUNNER_SPEC.loader.exec_module(benchmark_runner)


class StdlibBoundaryManifestTest(unittest.TestCase):
    def test_loadable_modules_have_one_boundary_entry(self) -> None:
        manifest = load_manifest(ROOT)
        self.assertEqual(loadable_modules(ROOT), set(manifest.by_name))
        self.assertEqual([], check_manifest(ROOT))

    def test_source_only_module_declares_no_native_entries(self) -> None:
        manifest = load_manifest(ROOT)
        for name in ("base64", "compress", "csv"):
            module = manifest.by_name[name]
            self.assertIn(name, source_modules(ROOT))
            self.assertNotIn(name, native_entry_binder_modules(ROOT))
            self.assertEqual([], module.get("public_native", []))

    def test_module_native_entries_are_bound_by_a_generated_binder(self) -> None:
        manifest = load_manifest(ROOT)
        os_module = manifest.by_name["os"]
        binders = native_entry_binder_modules(ROOT)
        # Every module with generated native entries belongs here. Keep the
        # complete set explicit so a newly generated binder cannot silently
        # bypass the generic loader's boundary inventory.
        self.assertEqual(
            {
                "cluster",
                "crypto",
                "http2",
                "io",
                "math",
                "mem",
                "net",
                "os",
                "regex",
                "runtime",
                "sync",
                "sys",
                "test_yield",
                "time",
            },
            set(binders),
        )
        self.assertIn("os", source_modules(ROOT))
        self.assertEqual(
            "xr_stdlib_vm_bind_os_generated",
            binders.get("os"),
        )
        self.assertTrue(os_module.get("private_native_sources"))

    def test_no_module_is_loaded_by_a_c_factory(self) -> None:
        """The loader is generic: no module may have a C factory written for it."""
        loader = (ROOT / "src/module/xmodule.c").read_text(encoding="utf-8")
        self.assertNotIn("xr_native_module_create_", loader)
        self.assertFalse(list(ROOT.glob("stdlib/*/*.c*")) and [
            path
            for path in ROOT.glob("stdlib/**/*.c")
            if "xr_native_module_create_" in path.read_text(encoding="utf-8")
        ])
        for module in load_manifest(ROOT).modules:
            self.assertNotIn("factory_source", module)
            self.assertNotIn("factory_symbol", module)

    def test_loader_declarations_must_name_something_real(self) -> None:
        manifest = load_manifest(ROOT)
        raw = copy.deepcopy(manifest.raw)
        sync_module = next(module for module in raw["module"] if module["name"] == "sync")
        sync_module["native_type_exports"] = [
            {"name": "Semaphore", "builtin_type": "XR_TNOSUCHTYPE"}
        ]
        mutated = BoundaryManifest(
            root=ROOT,
            raw=raw,
            modules=tuple(raw["module"]),
            vm_fastpaths=tuple(raw.get("vm_fastpath", ())),
        )
        with mock.patch("check_stdlib_boundary.load_manifest", return_value=mutated):
            errors = check_manifest(ROOT)
        self.assertIn(
            "module sync: native_type_exports 'Semaphore' names an unknown builtin type "
            "XR_TNOSUCHTYPE",
            errors,
        )

    def test_duplicate_private_leaf_provider_identity_fails_generation(self) -> None:
        stdlibgen = load_stdlibgen(ROOT)
        entries, *_ = stdlibgen.parse_def_metadata(ROOT)
        original = next(entry for entry in entries if entry.vm)
        divergent = dataclasses.replace(original, vm=f"{original.vm}_divergent")
        with self.assertRaisesRegex(SystemExit, "duplicate VM binding rows disagree"):
            stdlibgen.unique_vm_binding_entries([original, divergent])

    def test_target_leaf_source_owner_is_private_unique_and_canonical(self) -> None:
        stdlibgen = load_stdlibgen(ROOT)
        owners: dict[str, str] = {}
        stdlibgen.validate_target_leaf_source_owner(
            ROOT, "os", "__getpid", "i64-getpid", "internal", "nothrow", "no_heap", owners
        )
        self.assertEqual({"i64-getpid": "os.__getpid"}, owners)
        with self.assertRaisesRegex(SystemExit, "must have internal visibility"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "publicLeaf", "i64-public", "public", "nothrow", "no_heap", {}
            )
        with self.assertRaisesRegex(SystemExit, "must declare effect = nothrow"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "__missingEffect", "i64-missing-effect", "internal", "", "no_heap", {}
            )
        with self.assertRaisesRegex(SystemExit, "must declare allocation = no_heap"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "__missingAllocation", "i64-missing-allocation", "internal", "nothrow", "", {}
            )
        with self.assertRaisesRegex(SystemExit, "duplicate providers"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "os", "anotherPrivateLeaf", "i64-getpid", "internal", "nothrow", "no_heap", owners
            )
        with self.assertRaisesRegex(SystemExit, "requires canonical source module"):
            stdlibgen.validate_target_leaf_source_owner(
                ROOT, "missing_source_module", "__leaf", "i64-missing", "internal", "nothrow", "no_heap", {}
            )

    def test_semantic_native_and_fastpath_contracts_are_source_derived(self) -> None:
        self.assertEqual([], check_semantic_owners(ROOT))
        self.assertEqual([], check_fastpaths(ROOT))

    def test_regex_object_and_literal_policy_are_source_owned(self) -> None:
        source = (ROOT / "stdlib/regex/regex.xr").read_text(encoding="utf-8")
        binding = (ROOT / "stdlib/regex/xregex_binding.c").read_text(encoding="utf-8")
        definitions = (ROOT / "stdlib/defs/core.def").read_text(encoding="utf-8")
        canonicalizer = (ROOT / "src/frontend/canonical/xcanon.c").read_text(encoding="utf-8")
        self.assertIn("export final class Regex {", source)
        self.assertIn("constructor(pattern: string, flags: string", source)
        self.assertIn("fn rxParseFlags(", source)
        self.assertIn("this.prog = Array<i64>()", source)
        self.assertIn('xr_ast_new_expr(ctx->session, "regex", "Regex"', canonicalizer)
        for retired in (
            "make_regex",
            "xr_regex_compile_literal",
            "xr_regex_register_class",
            "regex_parse_flags",
            "regex_compile",
        ):
            self.assertNotIn(retired, binding)
        self.assertNotIn("class Regex {", definitions)
        self.assertNotIn("fn __regexNew {", definitions)
        self.assertNotIn("fn __regexParseFlags {", definitions)
        self.assertFalse((ROOT / "stdlib/types/regex.xr").exists())

    def test_regex_match_shape_is_source_owned(self) -> None:
        source = (ROOT / "stdlib/regex/regex.xr").read_text(encoding="utf-8")
        binding = (ROOT / "stdlib/regex/xregex_binding.c").read_text(encoding="utf-8")
        definitions = (ROOT / "stdlib/defs/core.def").read_text(encoding="utf-8")
        self.assertIn("export final class RegexMatch {", source)
        for field in ("start: i64", "end: i64", "text: string", "groups: Array<string>"):
            self.assertIn(field, source)
        for legacy_owner in ("XR_REGEX_MATCH_FIELD_", "regex_match_new", "__regexMatchNew"):
            self.assertNotIn(legacy_owner, binding)
            self.assertNotIn(legacy_owner, definitions)

    def test_os_process_surface_has_one_source_model(self) -> None:
        source = (ROOT / "stdlib/os/os.xr").read_text(encoding="utf-8")
        definitions = (ROOT / "stdlib/defs/core.def").read_text(encoding="utf-8")
        binding = (ROOT / "stdlib/os/os.c").read_text(encoding="utf-8")
        aot = (ROOT / "src/aot/xrt_os.h").read_text(encoding="utf-8")
        sys_source = (ROOT / "stdlib/sys/sys.xr").read_text(encoding="utf-8")
        sys_provider = (ROOT / "src/coro/xsys_provider.c").read_text(encoding="utf-8")
        sys_aot = (ROOT / "src/aot/xrt_sys.h").read_text(encoding="utf-8")
        self.assertNotIn("export fn spawn(", source)
        self.assertNotIn("fn __spawn {", definitions)
        self.assertNotIn("os_spawn", binding)
        self.assertIn("static spawn(program: string", sys_source)
        self.assertIn("var code = this.tryWait()", sys_source)
        self.assertIn("time.sleep(1)", sys_source)
        self.assertNotIn("fn __processWait {", definitions)
        self.assertNotIn("xr_sys_provider_process_wait", sys_provider)
        self.assertNotIn("xrt_sys_process_wait", sys_aot)
        self.assertEqual(1, definitions.count("  fn __kill {"))
        self.assertNotIn("xrt_os_kill(XrValue", aot)
        self.assertIn("if (count > 0) { return count }", source)

    def test_http2_transport_policy_is_source_owned(self) -> None:
        source = (ROOT / "stdlib/http2/http2.xr").read_text(encoding="utf-8")
        binding = (ROOT / "stdlib/http2/http2_binding.c").read_text(encoding="utf-8")
        definitions = (ROOT / "stdlib/defs/core.def").read_text(encoding="utf-8")
        aot = (ROOT / "src/aot/xrt_http.h").read_text(encoding="utf-8")
        cache = (ROOT / "src/module/xstdlib_runtime_cache.h").read_text(encoding="utf-8")

        for owner in (
            'const H2_ALPN = "h2"',
            "const MAX_TRANSPORT_TIMEOUT_MS: i64 = 2147483647",
            "const CONNECT_TLS_UNAVAILABLE: i64 = -2",
            "const CONNECT_PROTOCOL_MISMATCH: i64 = -3",
            "while (written < len(ex.outbox))",
            "var count = __send(handle, ex.outbox, written)",
            "throw Http2Error.TlsUnavailable",
            'throw Http2Error.FramingError { reason: "peer did not negotiate HTTP/2" }',
        ):
            self.assertIn(owner, source)

        for removed_c_policy in (
            "ALPN_PROTOS",
            "XR_H2_MAX_READ",
            "h2_conn_write_all",
            'xr_hashmap_get(registry->loaded_modules, "http2")',
        ):
            self.assertNotIn(removed_c_policy, binding)
        self.assertIn("void *http2_state;", cache)

        for unavailable in (
            "xrt_http_h2_connect_unavailable",
            "xrt_http_h2_send_unavailable",
            "xrt_http_h2_recv_unavailable",
            "xrt_http_h2_close_unavailable",
        ):
            self.assertNotIn(unavailable, definitions)
            self.assertNotIn(unavailable, aot)
        self.assertIn('signature: "(handle: i64, data: Array<u8>, offset: i64): i64"', definitions)
        self.assertIn('signature: "(handle: i64): ()"', definitions)

    def test_exact_aot_runtime_adapters_do_not_allow_module_helper_families(self) -> None:
        manifest = load_manifest(ROOT)
        allowed = {
            name: set(module.get("aot_runtime_adapters", ()))
            for name, module in manifest.by_name.items()
            if module.get("aot_runtime_adapters")
        }
        self.assertEqual(
            [],
            check_forbidden_tokens(ROOT, manifest.aot_helper_forbidden_modules, allowed),
        )

    def test_superseded_global_result_cannot_reenter_prelude(self) -> None:
        self.assertEqual([], check_error_model_policy(ROOT))

    def test_dynamic_surface_is_fully_classified(self) -> None:
        errors, report = check_dynamic(ROOT)
        self.assertEqual([], errors)
        self.assertEqual(report["allowed_count"], len(report["allowlist"]))
        for entry in report["allowlist"]:
            self.assertEqual(
                {"symbol", "direction", "domain", "reason", "owner", "review_task"},
                set(entry),
            )
        self.assertEqual(0, report["migration_debt_count"])

    def test_policy_agreement_is_reported_after_all_blockers_clear(self) -> None:
        errors, report = build_report(ROOT)
        self.assertEqual([], errors)
        self.assertTrue(report["status"]["consistent"])
        self.assertTrue(report["status"]["policy_agreement"])
        self.assertEqual([], report["status"]["policy_agreement_blockers"])

    def test_contract_and_benchmark_backends_must_agree_exactly(self) -> None:
        contracts = [{"module": "regex", "backends": ["vm"]}]
        matching = [
            {"id": "regex.escape.contract", "module": "regex", "compare": ["vm"]}
        ]
        self.assertEqual([], benchmark_backend_agreement(contracts, matching))
        mismatches = benchmark_backend_agreement(
            contracts,
            [
                {
                    "id": "regex.escape.contract",
                    "module": "regex",
                    "compare": ["vm", "aot"],
                }
            ],
        )
        self.assertEqual(["vm"], mismatches[0]["contract_backends"])
        self.assertEqual(["vm", "aot"], mismatches[0]["benchmark_backends"])

    def test_vm_benchmark_source_and_oracle_must_match_contract_diff_case(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xt_stdlib_oracle_agreement.") as tmp:
            root = Path(tmp)
            source = "cases/regex.xr"
            manifest = root / "regex_cases.txt"
            manifest.write_text(f"{source}\n", encoding="utf-8")
            (root / "cases").mkdir()
            (root / f"{source}.expected").write_bytes(b"escaped\n")
            contracts = [
                {
                    "module": "regex",
                    "backends": ["vm"],
                    "diff_cases_manifest": "regex_cases.txt",
                }
            ]
            benchmark = {
                "id": "regex.escape.contract",
                "module": "regex",
                "compare": ["vm"],
                "source": source,
                "output_oracle": f"{source}.expected",
            }
            self.assertEqual(
                [], benchmark_oracle_agreement(root, contracts, [benchmark])
            )
            bad_oracle = {**benchmark, "output_oracle": "cases/wrong.expected"}
            mismatch = benchmark_oracle_agreement(root, contracts, [bad_oracle])
            self.assertIn("output_oracle", mismatch[0]["reasons"][0])
            bad_source = {**benchmark, "source": "cases/other.xr"}
            mismatch = benchmark_oracle_agreement(root, contracts, [bad_source])
            self.assertTrue(
                any("absent from contract" in reason for reason in mismatch[0]["reasons"])
            )

    def test_vm_only_cases_have_finite_timeout_and_allow_success_stderr(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xt_stdlib_vm.") as tmp:
            root = Path(tmp)
            case = root / "case.xr"
            case.write_text("print(1)\n", encoding="utf-8")
            Path(f"{case}.expected").write_bytes(b"1\n")
            Path(f"{case}.args").write_text('--mode "two words"\n', encoding="utf-8")
            manifest = root / "cases.txt"
            manifest.write_text("case.xr\n", encoding="utf-8")
            contract = {"diff_cases_manifest": "cases.txt"}
            completed = subprocess.CompletedProcess(
                args=["xray"], returncode=0, stdout=b"1\n", stderr=b"diagnostic\n"
            )
            with mock.patch.object(
                stdlib_migration.subprocess, "run", return_value=completed
            ) as run, contextlib.redirect_stdout(io.StringIO()):
                status = stdlib_migration.run_vm_cases(root, contract, root / "xray")
        self.assertEqual(0, status)
        self.assertEqual(
            stdlib_migration.VM_CASE_TIMEOUT_SECONDS,
            run.call_args.kwargs["timeout"],
        )
        self.assertEqual(
            [str(root / "xray"), "run", str(case), "--", "--mode", "two words"],
            run.call_args.args[0],
        )
        self.assertEqual(b"", run.call_args.kwargs["input"])

    def test_vm_only_case_timeout_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xt_stdlib_vm_timeout.") as tmp:
            root = Path(tmp)
            case = root / "case.xr"
            case.write_text("print(1)\n", encoding="utf-8")
            Path(f"{case}.expected").write_bytes(b"1\n")
            (root / "cases.txt").write_text("case.xr\n", encoding="utf-8")
            with mock.patch.object(
                stdlib_migration.subprocess,
                "run",
                side_effect=subprocess.TimeoutExpired(["xray"], 120),
            ), contextlib.redirect_stderr(io.StringIO()):
                status = stdlib_migration.run_vm_cases(
                    root, {"diff_cases_manifest": "cases.txt"}, root / "xray"
                )
        self.assertEqual(1, status)

    def test_vm_only_case_requires_exit_zero_and_byte_exact_stdout(self) -> None:
        failures = (
            subprocess.CompletedProcess(
                args=["xray"], returncode=1, stdout=b"1\n", stderr=b"failed\n"
            ),
            subprocess.CompletedProcess(
                args=["xray"], returncode=0, stdout=b"01\n", stderr=b""
            ),
        )
        for completed in failures:
            with self.subTest(returncode=completed.returncode, stdout=completed.stdout):
                with tempfile.TemporaryDirectory(prefix="xt_stdlib_vm_red.") as tmp:
                    root = Path(tmp)
                    case = root / "case.xr"
                    case.write_text("print(1)\n", encoding="utf-8")
                    Path(f"{case}.expected").write_bytes(b"1\n")
                    (root / "cases.txt").write_text("case.xr\n", encoding="utf-8")
                    with mock.patch.object(
                        stdlib_migration.subprocess, "run", return_value=completed
                    ), contextlib.redirect_stderr(io.StringIO()):
                        status = stdlib_migration.run_vm_cases(
                            root,
                            {"diff_cases_manifest": "cases.txt"},
                            root / "xray",
                        )
                self.assertEqual(1, status)

    def test_benchmark_schema_two_enforces_contract_backend_agreement(self) -> None:
        boundary = mock.Mock()
        boundary.by_name = {"regex": {"perf_suite": "stdlib/regex"}}
        base = {
            "schema": 2,
            "governed_suites": ["stdlib/regex"],
            "benchmark": [
                {
                    "id": "regex.escape.contract",
                    "module": "regex",
                    "suite": "stdlib/regex",
                    "kind": "cpu_kernel",
                    "source": "case.xr",
                    "output_oracle": "case.xr.expected",
                    "warmup": 1,
                    "iterations": 1,
                    "quick_iterations": 1,
                    "metrics": ["wall_ns"],
                    "compare": ["vm"],
                }
            ],
        }
        with tempfile.TemporaryDirectory(prefix="xt_stdlib_bench_manifest.") as tmp:
            fixture_root = Path(tmp)
            (fixture_root / "case.xr").write_text("print(1)\n", encoding="utf-8")
            (fixture_root / "case.xr.expected").write_bytes(b"1\n")
            with mock.patch.object(
                benchmark_runner, "ROOT", fixture_root
            ), mock.patch.object(
                benchmark_runner, "load_manifest", return_value=boundary
            ), mock.patch.object(
                benchmark_runner, "contract_modules", return_value=["regex"]
            ), mock.patch.object(
                benchmark_runner,
                "validate_contract",
                return_value=([], {"backends": ["vm"]}),
            ):
                self.assertEqual([], benchmark_runner.validate_manifest(base))
                missing_oracle = copy.deepcopy(base)
                del missing_oracle["benchmark"][0]["output_oracle"]
                self.assertTrue(
                    any(
                        "requires output_oracle" in error
                        for error in benchmark_runner.validate_manifest(missing_oracle)
                    )
                )
                bad_oracle = copy.deepcopy(base)
                bad_oracle["benchmark"][0]["output_oracle"] = "missing.expected"
                self.assertTrue(
                    any(
                        "output_oracle must be case.xr.expected" in error
                        for error in benchmark_runner.validate_manifest(bad_oracle)
                    )
                )
                (fixture_root / "wrong.expected").write_bytes(b"1\n")
                wrong_existing_oracle = copy.deepcopy(base)
                wrong_existing_oracle["benchmark"][0]["output_oracle"] = "wrong.expected"
                self.assertTrue(
                    any(
                        "output_oracle must be case.xr.expected" in error
                        for error in benchmark_runner.validate_manifest(wrong_existing_oracle)
                    )
                )
                schema_one = copy.deepcopy(base)
                schema_one["schema"] = 1
                self.assertIn(
                    "benchmark manifest schema must be 2",
                    benchmark_runner.validate_manifest(schema_one),
                )
                dual = copy.deepcopy(base)
                dual["benchmark"][0]["compare"] = ["vm", "aot"]
                dual["benchmark"][0]["vm_budget_ratio"] = 80.0
                self.assertTrue(
                    any(
                        "disagrees with" in error
                        for error in benchmark_runner.validate_manifest(dual)
                    )
                )
                stale_budget = copy.deepcopy(base)
                stale_budget["benchmark"][0]["vm_budget_ratio"] = 80.0
                self.assertTrue(
                    any(
                        "only valid for VM/AOT" in error
                        for error in benchmark_runner.validate_manifest(stale_budget)
                    )
                )
                aot_only = copy.deepcopy(base)
                aot_only["benchmark"][0]["compare"] = ["aot"]
                self.assertTrue(
                    any(
                        "must be ['vm'] or ['vm', 'aot']" in error
                        for error in benchmark_runner.validate_manifest(aot_only)
                    )
                )

    def test_vm_only_benchmark_never_builds_or_runs_aot(self) -> None:
        entry = {
            "id": "regex.escape.contract",
            "module": "regex",
            "kind": "cpu_kernel",
            "source": "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr",
            "warmup": 1,
            "iterations": 2,
            "quick_iterations": 1,
            "compare": ["vm"],
            "output_oracle": (
                "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr.expected"
            ),
        }
        with tempfile.TemporaryDirectory(prefix="xt_stdlib_bench.") as tmp:
            fixture_root = Path(tmp)
            oracle = (
                fixture_root
                / "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr.expected"
            )
            oracle.parent.mkdir(parents=True)
            oracle.write_bytes(b"ok\n")
            with mock.patch.object(
                benchmark_runner, "ROOT", fixture_root
            ), mock.patch.object(
                benchmark_runner.subprocess, "run"
            ) as native_build, mock.patch.object(
                benchmark_runner, "run_sample", return_value=(0, b"ok\n", b"", 10)
            ) as sample:
                result = benchmark_runner.execute(
                    entry, Path("/xray"), False, fixture_root
                )
        native_build.assert_not_called()
        self.assertEqual(3, sample.call_count)
        self.assertEqual(["vm"], result["backends"])
        self.assertEqual({"vm"}, set(result["samples_ns"]))
        self.assertEqual(
            "tests/diff/cases/semantics/stdlib/regex_escape_direct.xr.expected",
            result["output_oracle"],
        )
        self.assertNotIn("aot_binary_size_bytes", result)
        self.assertNotIn("vm_aot_ratio", result)

    def test_vm_only_benchmark_fails_on_oracle_drift(self) -> None:
        entry = {
            "id": "regex.escape.contract",
            "module": "regex",
            "kind": "cpu_kernel",
            "source": "regex.xr",
            "warmup": 1,
            "iterations": 1,
            "quick_iterations": 1,
            "compare": ["vm"],
            "output_oracle": "regex.xr.expected",
        }
        with tempfile.TemporaryDirectory(prefix="xt_stdlib_bench_drift.") as tmp:
            fixture_root = Path(tmp)
            (fixture_root / "regex.xr.expected").write_bytes(b"expected\n")
            with mock.patch.object(
                benchmark_runner, "ROOT", fixture_root
            ), mock.patch.object(
                benchmark_runner, "run_sample", return_value=(0, b"drifted\n", b"", 10)
            ):
                with self.assertRaisesRegex(RuntimeError, "byte-exact oracle"):
                    benchmark_runner.execute(entry, Path("/xray"), False, fixture_root)

    def test_benchmark_runner_invariants_fail_when_each_fact_is_removed(self) -> None:
        source = (ROOT / "tests/benchmarks/stdlib/run.py").read_text(encoding="utf-8")
        self.assertEqual([], check_runner_source(source))
        removals = {
            "validated_resolved_backends": (
                '        if compare not in (["vm"], ["vm", "aot"]):\n',
                "",
            ),
            "observable_equality_only_for_multiple_outputs": (
                "    if len(outputs) > 1 and len(set(outputs.values())) != 1:\n",
                "",
            ),
            "selected_vm_output_hash": (
                '        "output_sha256": hashlib.sha256(outputs["vm"]).hexdigest(),\n',
                "",
            ),
            "vm_only_byte_exact_output_oracle": (
                "            expected_output = oracle_path.read_bytes()\n",
                "",
            ),
        }
        for invariant, (old, new) in removals.items():
            with self.subTest(invariant=invariant):
                self.assertIn(old, source)
                self.assertIn(invariant, check_runner_source(source.replace(old, new, 1)))
        oracle_equality = "            elif output_oracle != expected_oracle:\n"
        self.assertIn(oracle_equality, source)
        self.assertIn(
            "vm_only_byte_exact_output_oracle",
            check_runner_source(source.replace(oracle_equality, "", 1)),
        )

    def test_benchmark_runner_rejects_false_aot_requirements_for_vm_only(self) -> None:
        source = (ROOT / "tests/benchmarks/stdlib/run.py").read_text(encoding="utf-8")
        mutations = {
            "aot_build_requires_selected_aot_backend": (
                '    if "aot" in backends:\n        build = subprocess.run(',
                "    if True:\n        build = subprocess.run(",
            ),
            "aot_size_requires_selected_aot_backend": (
                '    if "aot" in backends:\n        result["aot_binary_size_bytes"] = ',
                '    if True:\n        result["aot_binary_size_bytes"] = ',
            ),
            "vm_aot_ratio_requires_dual_mode": (
                '    if backends == ["vm", "aot"]:\n        ratio = ',
                "    if True:\n        ratio = ",
            ),
        }
        for invariant, (old, new) in mutations.items():
            with self.subTest(invariant=invariant):
                self.assertIn(old, source)
                self.assertIn(invariant, check_runner_source(source.replace(old, new, 1)))


if __name__ == "__main__":
    unittest.main()
