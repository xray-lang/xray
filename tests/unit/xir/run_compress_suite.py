"""Run all original compression tests in VM and the matching generated C."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


SOURCE = "tests/regression/10_stdlib/1301_compress.xr"


def execute(command, root, directory, name):
    record = {"command": [str(value) for value in command], "cwd": str(root)}
    try:
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=550)
        (directory / (name + ".stdout")).write_bytes(result.stdout)
        (directory / (name + ".stderr")).write_bytes(result.stderr)
        record["exitCode"] = result.returncode
        if result.returncode or result.stderr:
            raise RuntimeError(f"{name} failed; see {directory}")
    except subprocess.TimeoutExpired as error:
        (directory / (name + ".stdout")).write_bytes(error.stdout or b"")
        (directory / (name + ".stderr")).write_bytes(error.stderr or b"")
        record["timedOut"] = True
        raise
    finally:
        (directory / (name + ".command.json")).write_text(
            json.dumps(record, indent=2) + "\n", encoding="utf-8")


def validate(report, backend, names, source_digest):
    expected_keys = {"schema", "backend", "source", "sourceSha256", "emitBodySha256", "entries",
                     "discovered", "executed", "passed", "skipped", "failed"}
    if set(report) != expected_keys or report["schema"] != 1 or report["backend"] != backend:
        raise ValueError(f"{backend}: wrong report shape")
    if report["source"] != SOURCE or report["sourceSha256"] != source_digest:
        raise ValueError(f"{backend}: source identity differs")
    if not re.fullmatch("[0-9a-f]{64}", report["emitBodySha256"]):
        raise ValueError(f"{backend}: invalid emitted C identity")
    counts = ("discovered", "executed", "passed", "skipped", "failed")
    if any(type(report[key]) is not int for key in counts) or tuple(report[key] for key in counts) != (26, 26, 26, 0, 0):
        raise ValueError(f"{backend}: not all original tests actually passed")
    entries = report["entries"]
    if not isinstance(entries, list) or len(entries) != 26 or [row["name"] for row in entries] != names:
        raise ValueError(f"{backend}: original test names differ")
    ids = set()
    entry_keys = {"name", "function", "role", "timeout", "executed", "passed", "unitResult", "terminalStatus"}
    for row in entries:
        if set(row) != entry_keys or type(row["function"]) is not int or not 0 <= row["function"] <= 0xffffffff or row["function"] in ids:
            raise ValueError(f"{backend}: invalid or repeated catalog function")
        ids.add(row["function"])
        if (row["role"], row["timeout"], row["terminalStatus"]) != (1, 0, 1) or any(
                row[key] is not True for key in ("executed", "passed", "unitResult")):
            raise ValueError(f"{backend}: {row['name']} was not a returned Unit test")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("root", "vm", "native", "generated-c", "reports"):
        parser.add_argument("--" + option, required=True, type=Path)
    args = parser.parse_args()
    root, generated = args.root.resolve(), args.generated_c.resolve(strict=True)
    reports = args.reports.resolve()
    reports.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="run-", dir=reports))
    source_digest = hashlib.sha256((root / SOURCE).read_bytes()).hexdigest()
    names = re.findall(r'^\s+"(test_[a-zA-Z0-9_]+)"',
                       Path(__file__).with_name("compress_suite_inventory.h").read_text(encoding="utf-8"), re.M)
    if len(names) != 26 or len(set(names)) != 26:
        raise ValueError("independent inventory must contain all 26 distinct tests")
    original = generated.read_bytes()
    vm_report, native_report = directory / "vm.json", directory / "native.json"
    execute([args.vm.resolve(), root, generated, vm_report], root, directory, "vm")
    if generated.read_bytes() != original:
        raise ValueError("VM validation changed the native compiler input")
    execute([args.native.resolve(), native_report], root, directory, "native")
    vm, native = (json.loads(path.read_text(encoding="utf-8")) for path in (vm_report, native_report))
    validate(vm, "VM", names, source_digest)
    validate(native, "native", names, source_digest)
    if vm["emitBodySha256"] != native["emitBodySha256"] or vm["entries"] != native["entries"]:
        raise ValueError("VM/native catalog or emitted C identity differs")
    print(f"compress: original 26/26 VM and 26/26 native, no skips; reports {directory}")


if __name__ == "__main__":
    main()
