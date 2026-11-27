"""Check one Source catalog through bytecode and the C emitted by that same invocation."""

from pathlib import Path
import argparse
import hashlib
import json
import os
import tempfile

from test_lir_test_role_startup import invoke
from test_lir_pipeline_eval import compile_command


SOURCE = Path("demos/07-advanced/testing.xr")
CRYPTO_SOURCES = (
    "tests/regression/10_stdlib/1400_crypto_hash.xr",
    "tests/regression/10_stdlib/1401_crypto_hmac.xr",
    "tests/regression/10_stdlib/1402_crypto_random.xr",
    "tests/regression/10_stdlib/1403_crypto_sha512.xr",
    "tests/regression/10_stdlib/1405_crypto_utils.xr",
)
HELPER = b'''export fn initialValue() -> i64 {
    return 41
}

@test
fn imported_test() {
    assert(false, "the imported case entered the guest")
}
'''
# Specialization removes the generic declaration before these three functions.
# Checked has IDs 3, 4 and 5. Closed has IDs 2, 3 and 4.
ENTRIES = [("z_first_test", 2, 1, 0), ("skipped_test", 3, 2, 0), ("a_second_test", 4, 1, 0)]


def native_executable(args, root, generated, label, *, optimize=True):
    executable = root / (label + (".exe" if args.kind == "msvc" else "-native"))
    command = compile_command(args, generated, executable)
    if not optimize:
        enabled, disabled = ("/O2", "/Od") if args.kind == "msvc" else ("-O2", "-O0")
        command[command.index(enabled)] = disabled
    tests = args.source_root / "tests/lir"
    command[command.index(generated):command.index(generated)] = [
        ("/I" if args.kind == "msvc" else "-I") + str(tests),
        tests / "lir_test_suite_native.c",
    ]
    if args.kind == "msvc":
        command = ["/Fo" + str(root) + os.sep if isinstance(arg, str) and arg.startswith("/Fo")
                   else arg for arg in command]
        command.insert(1, "/D_CRT_SECURE_NO_WARNINGS")
    compilation = invoke(root, label + "-compile", command)
    assert compilation[0] == 0, (compilation, root)
    return executable


def catalog(path, source, raw, expected_counts):
    report = json.loads(path.read_bytes())
    assert set(report) == {"schema", "source", "sourceSha256", "entries", "passed", "skipped", "failed"}, report
    assert type(report["schema"]) is int and report["schema"] == 1, report
    assert report["source"] == source, report
    assert report["sourceSha256"] == hashlib.sha256(raw).hexdigest(), report
    assert all(type(report[key]) is int for key in ("passed", "skipped", "failed")), report
    assert tuple(report[key] for key in ("passed", "skipped", "failed")) == expected_counts, report
    rows = report["entries"]
    assert all(set(row) == {"name", "function", "role", "timeout"} for row in rows), report
    assert all(type(row[key]) is int for row in rows for key in ("function", "role", "timeout")), report
    assert len({row["function"] for row in rows}) == len(rows), report
    return report


def vm_products(args, work, root, source):
    generated = work / "suite.c"
    report = work / "vm.json"
    result = invoke(work, "suite-vm", [args.vm_driver, root, args.source_root / "stdlib", source, generated, report])
    return result, generated, report


def vm_suite(args, root, raw, helper=HELPER):
    source = root / SOURCE
    source.parent.mkdir(parents=True)
    source.write_bytes(raw)
    source.with_name("suite-catalog-helper.xr").write_bytes(helper)
    result, generated, report = vm_products(args, root, root, SOURCE.as_posix())
    return result, source, generated, report


def check_suite(args, root, raw, failing=False):
    vm, source, generated, vm_report = vm_suite(args, root, raw)
    assert generated.is_file() and generated.stat().st_size, (vm, root)
    counts = (1, 1, 1) if failing else (2, 1, 0)
    result = catalog(vm_report, SOURCE.as_posix(), raw, counts)
    assert [(row["name"], row["function"], row["role"], row["timeout"]) for row in result["entries"]] == ENTRIES
    # Native must use the catalog baked into the emitted C. The original
    # Source file is no longer available during native compilation or execution.
    source.unlink()
    binary = native_executable(args, root, generated, "suite")
    native_report = root / "native.json"
    native = invoke(root, "suite-native", [binary, native_report])
    assert catalog(native_report, SOURCE.as_posix(), raw, counts) == result, root
    assert native_report.read_bytes() == vm_report.read_bytes(), root
    assert native[2] == vm[2], (native, vm, root)
    for run, backend in ((vm, b"VM"), (native, b"native")):
        second = b"a_second_test FAIL" if failing else b"a_second_test PASS"
        summary = b"original catalog: 1 PASS, 1 SKIP, 1 FAIL" if failing else b"original catalog: 2 PASS, 1 SKIP"
        assert run[0] == int(failing) and run[1].splitlines() == [
            backend + b" z_first_test PASS", backend + b" " + second, summary,
        ], (run, root)
        if failing:
            assert b"[Uncaught Panic] E0445: suite state did not persist" in run[2], (run, root)
        else:
            assert not run[2], (run, root)
    return result, generated


def check_rejected(args, root, raw, failure, helper=HELPER):
    vm, _, generated, report = vm_suite(args, root, raw, helper)
    assert vm[0] == 1 and not vm[1] and failure in vm[2], (vm, root)
    assert not generated.exists() and not report.exists(), root


def check_nullable_string_comparisons(args, work):
    work.mkdir()
    source = "nullable-string-comparisons.xr"
    raw = b'''fn xrt_nullable_string_equal(left: string?, right: string?) -> bool {
    return false
}

fn make_text(value: string) -> string? {
    return value + "!"
}

@test
fn compare_nullable_strings() {
    const shared = make_text("live")
    const other = make_text("other")
    assert(!xrt_nullable_string_equal(shared, shared))
    var numbers: Array<i64> = [11, 22]
    const view: Slice<i64> = numbers[0:2]
    const first = view[0]
    const equal = shared == shared
    const distinct = shared != other
    const second = view[1]
    assert(first == 11 && second == 22)
    assert(equal && distinct)
    assertEqual(shared, make_text("live"))
    assert(shared != null)
}
'''
    (work / source).write_bytes(raw)
    vm, generated, vm_report = vm_products(args, work, work, source)
    assert vm[0] == 0 and not vm[2], vm
    result = catalog(vm_report, source, raw, (1, 0, 0))
    assert [row["name"] for row in result["entries"]] == ["compare_nullable_strings"], result
    executable = native_executable(args, work, generated, "comparison")
    native_report = work / "native.json"
    native = invoke(work, "suite-native", [executable, native_report])
    assert native[0] == 0 and not native[2], native
    assert vm_report.read_bytes() == native_report.read_bytes(), work


def check_invoke_string_owners(args, work):
    work.mkdir()
    source = "invoke-string-owners.xr"
    raw = b'''enum ProbeError { Stop { detail: i64 } }

fn guarded(allow: bool) throws(ProbeError) -> i64 {
    if (!allow) { throw ProbeError.Stop { detail: 19 } }
    return 21
}

fn make(label: string) -> string {
    return label + "!"
}

fn exercise(allow: bool) {
    const first = make("first")
    const a = try! guarded(true)
    const second = make("second")
    const b = try! guarded(allow)
    assertEqual(first, "first!")
    assertEqual(second, "second!")
    assertEqual(a + b, 42)
}

@test
fn owners_reach_normal_successor() {
    exercise(true)
}

@test
fn owners_retire_on_error_successor() {
    exercise(false)
}
'''
    (work / source).write_bytes(raw)
    vm, generated, vm_report = vm_products(args, work, work, source)
    assert vm[0] == 1 and not vm[2], vm
    assert vm[1].splitlines() == [
        b"VM owners_reach_normal_successor PASS",
        b"VM owners_retire_on_error_successor FAIL",
        b"original catalog: 1 PASS, 0 SKIP, 1 FAIL",
    ], vm
    result = catalog(vm_report, source, raw, (1, 0, 1))
    assert [row["name"] for row in result["entries"]] == [
        "owners_reach_normal_successor", "owners_retire_on_error_successor",
    ], result
    executable = native_executable(args, work, generated, "invoke-owners")
    native_report = work / "native.json"
    native = invoke(work, "suite-native", [executable, native_report])
    assert native[0] == vm[0] and native[2] == vm[2], (native, vm)
    assert vm_report.read_bytes() == native_report.read_bytes(), work


def check_crypto_catalogs(args, root):
    manifest = json.loads((args.source_root / "tests/smoke/expected/crypto-catalog.json").read_bytes())
    by_source = {suite["source"]: suite for suite in manifest["suites"]}
    for source in CRYPTO_SOURCES:
        expected = by_source[source]
        path = args.source_root / source
        raw = path.read_bytes()
        assert hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest() == expected["sourceCanonicalSha256"], source
        work = root / path.stem
        work.mkdir()
        vm, generated, vm_report = vm_products(args, work, args.source_root, source)
        assert vm[0] == 0 and not vm[2], (vm, source)
        count = len(expected["entries"])
        report = catalog(vm_report, source, raw, (count, 0, 0))
        assert [{key: row[key] for key in ("name", "role", "timeout")} for row in report["entries"]] == expected["entries"]
        assert b"({" not in generated.read_bytes(), generated

        # Large generated Crypto suites need bounded compiler time under sanitizer instrumentation.
        binary = native_executable(args, work, generated, "suite", optimize=False)
        native_report = work / "native.json"
        native = invoke(work, "suite-native", [binary, native_report])
        assert native[0] == 0 and not native[2], (native, source)
        assert native_report.read_bytes() == vm_report.read_bytes(), source


def check_original_catalogs(args, root):
    translation = "1151_string_translate"
    expected_counts = {translation: 5, "1421_datetime_format": 13, "1422_datetime_calc": 21}
    translation_tests = [
        "test_translate_basic_ascii", "test_translate_partial", "test_translate_unicode",
        "test_translate_empty_table", "test_translate_with_reverse",
    ]
    for name, count in expected_counts.items():
        source = f"tests/regression/10_stdlib/{name}.xr"
        path = args.source_root / source
        raw = path.read_bytes()
        work = root / name
        work.mkdir()
        vm, generated, vm_report = vm_products(args, work, args.source_root, source)
        assert vm[0] == 0 and not vm[2], (vm, source)
        plan = catalog(vm_report, source, raw, (count, 0, 0))
        assert all(row["role"] == 1 and row["timeout"] == 0 for row in plan["entries"]), source
        if name == translation:
            assert [row["name"] for row in plan["entries"]] == translation_tests
            canonical = raw.replace(b"\r\n", b"\n")
            assert hashlib.sha256(canonical).hexdigest() == (
                "b51c56ce8f7735e9b92de4f91d459a600d5bb1cd7f4d36788491c249337c4f0a")
            assert b"({" not in generated.read_bytes(), generated

        binary = native_executable(args, work, generated, "suite", optimize=False)
        native_report = work / "native.json"
        native = invoke(work, "suite-native", [binary, native_report])
        assert native[0] == 0 and not native[2], (native, source)
        assert native_report.read_bytes() == vm_report.read_bytes(), source



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("vm-driver", "compiler", "include", "source-root", "work"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--library", type=Path, nargs="+", required=True)
    parser.add_argument("--kind", choices=("msvc", "gnu"), required=True)
    parser.add_argument("--msvc-runtime", default="MultiThreaded")
    parser.add_argument("--asan", action="store_true")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="suite-", dir=args.work.resolve()))
    fixture = args.source_root / "tests/lir/programs/test-suite-catalog.xr"
    raw = fixture.read_bytes().replace(b"\r\n", b"\n")

    lf, _ = check_suite(args, root / "lf", raw)
    crlf, generated = check_suite(args, root / "crlf", raw.replace(b"\n", b"\r\n"))
    assert lf["sourceSha256"] != crlf["sourceSha256"] and lf["entries"] == crlf["entries"], root
    check_suite(args, root / "panic", raw.replace(b"assert(state == 42", b"assert(state == 41"), failing=True)
    check_rejected(args, root / "deadline", raw.replace(b"@test\n", b"@test(timeout: 17)\n", 1),
                   b"test-role startup with a deadline")
    check_rejected(args, root / "no-runnable", b"@test(skip)\nfn skipped_test() {}\n",
                   b"suite requires a runnable root catalog")
    check_rejected(args, root / "initializer", raw, b"VM test action could not start",
                   helper=HELPER.replace(b"return 41", b'assert(false, "initializer failed")\n    return 41'))

    metadata = b", UINT32_C(2), UINT32_C(0), UINT32_C(4294967295)}"
    prefix, marker, rows = generated.read_bytes().partition(b"static const SuiteRow xrl_suite_rows[]")
    assert marker and rows.count(metadata) == 1, root
    mismatched = root / "crlf" / "mismatched.c"
    mismatched.write_bytes(prefix + marker + rows.replace(metadata, metadata.replace(b"4294967295", b"0")))
    binary = native_executable(args, root / "crlf", mismatched, "mismatched")
    reject_report = root / "crlf" / "reject.json"
    reject = invoke(root / "crlf", "mismatched-native", [binary, reject_report])
    assert reject[:2] == (1, b"") and reject[2].splitlines() == [
        b"generated suite catalog does not match its action table",
    ], (reject, root)
    assert not reject_report.exists(), root
    missing_work = root / "missing-source"
    missing_work.mkdir()
    missing, generated, report = vm_products(args, missing_work, root, "missing.xr")
    assert missing[0] == 1 and b"missing.xr: suite compilation failed" in missing[2], missing
    assert not generated.exists() and not report.exists(), missing_work

    check_nullable_string_comparisons(args, root / "nullable-string-comparisons")
    check_invoke_string_owners(args, root / "invoke-string-owners")
    check_rejected(args, root / "string-map-return", b'''fn makeTable() -> Map<string, string> {
    return {"a": "b"}
}
@test
fn tableReturn() { assert(len(makeTable()) == 1) }
''', b"String Map function result")
    check_rejected(args, root / "string-map-indirect", b'''fn tableSize(table: Map<string, string>) -> i64 { return len(table) }
fn invokeSize(size: fn(Map<string, string>) -> i64) -> i64 { return size({"a": "b"}) }
@test
fn indirectTable() { assert(invokeSize(tableSize) == 1) }
''', b"String Map callable signature")
    check_crypto_catalogs(args, root)
    check_original_catalogs(args, root)
    print(f"Executed VM and generated C suite catalogs for LF, CRLF, a guest panic, Crypto, Text and Datetime. Artifacts at {root}")


if __name__ == "__main__":
    main()
