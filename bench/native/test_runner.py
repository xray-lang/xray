"""Exercise real references and verify that bad output cannot get a median."""

import argparse
import csv
import json
import os
import shutil
import statistics
import runpy
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def observe_process(pid: int):
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        def exited(timeout=0):
            try:
                return kernel.WaitForSingleObject(handle, timeout) == 0
            finally:
                kernel.CloseHandle(handle)
        return exited
    return lambda: process_exited(pid)


def process_exited(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    stat = Path(f"/proc/{pid}/stat")
    if stat.exists() and stat.read_text().split(")", 1)[1].strip().startswith("Z"):
        return True
    # A failing regression still cleans up the fixture it started.
    import signal
    os.kill(pid, signal.SIGKILL)
    return False


def results(process: subprocess.CompletedProcess) -> tuple[Path, list[dict]]:
    assert "results: " in process.stdout, process.stdout + process.stderr
    path = Path(process.stdout.split("results: ")[-1].strip())
    with path.open(newline="", encoding="utf-8") as stream:
        return path, list(csv.DictReader(stream))


def identity_change(command: list[str], reference: Path, output: Path) -> None:
    copied = output / ("mutable-reference" + reference.suffix)
    shutil.copy2(reference, copied)
    observed = output / "provisional-rows.json"
    script = """
import csv, json, pathlib, sys
case, n, binary, observed, source = sys.argv[1:]
n = int(n)
if case == 'integer_loop':
    rows = list(csv.DictReader((pathlib.Path(source).parent / 'results.csv').open()))
    pathlib.Path(observed).write_text(json.dumps(rows))
    with pathlib.Path(binary).open('ab') as stream:
        stream.write(b'identity regression probe')
if case == 'fib':
    a, b = 0, 1
    for _ in range(n): a, b = b, a + b
    value = a
else:
    groups, remainder = divmod(n, 97)
    value = groups * 4656 + remainder * (remainder - 1) // 2
sys.stdout.reconfigure(newline='\\n')
print(value)
"""
    candidates = output / "identity-candidate.json"
    candidates.write_text(json.dumps([{"name": "mutator", "command": [
        sys.executable, "-c", script, "{case}", "{n}", str(copied), str(observed), "{source}"
    ]}]), encoding="utf-8")
    process = subprocess.run(command + ["--reference", str(copied), "--samples", "1",
                             "--case", "fib", "--case", "integer_loop", "--candidates", str(candidates)],
                             capture_output=True, text=True, timeout=15)
    assert process.returncode == 1, process.stdout + process.stderr
    path, rows = results(process)
    affected = [row for row in rows if row["candidate"] == "c"]
    assert len(affected) == 2 and all(row["status"] == "binary-changed" and not row["median_ms"] for row in affected)
    provisional = json.loads(observed.read_text(encoding="utf-8"))
    assert provisional and all(not row["median_ms"] for row in provisional)
    assert all(row["status"] == "pending-identity" for row in provisional)
    metadata = json.loads(path.with_name("metadata.json").read_text(encoding="utf-8"))
    assert metadata["run_status"] == "invalid"
    candidate = next(item for item in metadata["candidates"] if item["name"] == "c")
    assert candidate["identity_status"] == "changed" and candidate["binary"] != candidate["binary_after"]
    samples = json.loads(path.with_name("samples.json").read_text(encoding="utf-8"))
    assert len(samples) == 6 and all(sample["status"] == "ok" for sample in samples)
    assert all(Path(sample["stdout"]).read_bytes() for sample in samples)


def relative_command(command: list[str], reference: Path, output: Path) -> None:
    launch = output / "different-launch-directory"
    tool = launch / "relative-tools" / reference.name
    tool.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(reference, tool)
    candidates = output / "relative-candidate.json"
    candidates.write_text(json.dumps([{"name": "relative", "command": [
        str(Path("relative-tools") / reference.name), "{case}", "{n}"
    ]}]), encoding="utf-8")
    process = subprocess.run(command + ["--samples", "1", "--case", "fib", "--candidates", str(candidates)],
                             cwd=launch, capture_output=True, text=True, timeout=15)
    assert process.returncode == 0, process.stdout + process.stderr
    path, rows = results(process)
    assert all(row["status"] == "ok" and row["median_ms"] for row in rows)
    metadata = json.loads(path.with_name("metadata.json").read_text(encoding="utf-8"))
    candidate = next(item for item in metadata["candidates"] if item["name"] == "relative")
    assert candidate["binary"]["path"] == candidate["command"][0] == str(tool.resolve())
    samples = json.loads(path.with_name("samples.json").read_text(encoding="utf-8"))
    assert next(item for item in samples if item["candidate"] == "relative")["command"][0] == str(tool.resolve())


def descendant_cleanup(runner: dict, output: Path) -> None:
    for mode in ("timeout", "exit"):
        folder = Path(tempfile.mkdtemp(prefix=f"descendants-{mode}-", dir=output))
        observers, errors = [], []
        def observe():
            try:
                deadline = time.monotonic() + 5
                for name in ("child", "leaf"):
                    path = folder / f"{name}.pid"
                    while not path.exists() or not path.read_text(encoding="ascii").isdigit():
                        if time.monotonic() >= deadline:
                            raise AssertionError(f"{mode}: {name} did not start")
                        time.sleep(0.01)
                    pid = int(path.read_text(encoding="ascii"))
                    observers.append((name, pid, observe_process(pid)))
                # The parent cannot exit until stable process handles exist.
                (folder / "observed").touch()
            except Exception as error:
                errors.append(error)
        watcher = threading.Thread(target=observe, daemon=True)
        watcher.start()
        start = time.monotonic()
        sample = runner["run_sample"]([sys.executable, str(HERE / "timeout_fixture.py"), mode, str(folder)],
                                      b"parent\n", 2, folder / "sample")
        watcher.join(timeout=1)
        assert not watcher.is_alive() and not errors, errors
        outcomes = [(name, pid, exited()) for name, pid, exited in observers]
        assert len(outcomes) == 2 and all(item[2] for item in outcomes), outcomes
        assert time.monotonic() - start < 7, sample
        assert sample["status"] == ("timeout" if mode == "timeout" else "ok"), sample
        assert Path(sample["stdout"]).read_bytes() == b"parent\n"
        assert Path(sample["stderr"]).read_bytes() == b"retained stderr\n"


def late_descendant(runner: dict, reference: Path, output: Path) -> None:
    if os.name != "nt":
        return
    import ctypes
    folder = Path(tempfile.mkdtemp(prefix="late-descendant-", dir=output))
    observers, errors = [], []

    def observe(name):
        path = folder / f"{name}.pid"
        deadline = time.monotonic() + 5
        while not path.exists() or not path.read_text(encoding="ascii").isdigit():
            if time.monotonic() >= deadline:
                raise AssertionError(f"late: {name} did not start")
            time.sleep(0.01)
        observers.append(observe_process(int(path.read_text(encoding="ascii"))))

    def observe_child():
        try:
            observe("child")
            (folder / "observed").touch()
        except Exception as error:
            errors.append(error)

    original_dll = ctypes.WinDLL
    def controlled_dll(name, *args, **kwargs):
        library = original_dll(name, *args, **kwargs)
        if name.lower() == "kernel32":
            original_terminate = library.TerminateJobObject
            def terminate(job, exit_code):
                original_terminate.argtypes = terminate.argtypes
                original_terminate.restype = terminate.restype
                # This real descendant starts after the helper's member census.
                (folder / "spawn").touch()
                observe("leaf")
                return original_terminate(job, exit_code)
            library.TerminateJobObject = terminate
        return library

    globals_ = runner["main"].__globals__
    original_run, original_argv = globals_["run_child"], sys.argv
    def controlled_run(command, **kwargs):
        if "late" not in command:
            return original_run(command, **kwargs)
        ctypes.WinDLL = controlled_dll
        try:
            return original_run(command, **kwargs)
        finally:
            ctypes.WinDLL = original_dll
    never = folder / "must-not-run"
    candidates = folder / "candidates.json"
    candidates.write_text(json.dumps([
        {"name": "late", "command": [sys.executable, str(HERE / "timeout_fixture.py"), "late", str(folder)]},
        {"name": "never", "command": [sys.executable, "-c", f"from pathlib import Path;Path({str(never)!r}).touch()"]},
    ]), encoding="utf-8")
    watcher = threading.Thread(target=observe_child, daemon=True)
    watcher.start()
    globals_["run_child"] = controlled_run
    sys.argv = [str(HERE / "run.py"), "--reference", str(reference), "--output", str(folder),
                "--quick", "--case", "fib", "--samples", "2", "--candidates", str(candidates)]
    try:
        code = runner["main"]()
    finally:
        sys.argv, globals_["run_child"] = original_argv, original_run
        watcher.join(timeout=1)
        # The runner aborted instead of claiming the new member's exit. The
        # test owns its handle and finishes fixture cleanup independently.
        exited = [observer(5000) for observer in observers]
    assert code == 1, "unverifiable cleanup must fail the run"
    assert not never.exists(), "another candidate ran after an unverified late-born descendant"
    assert not errors and not watcher.is_alive() and len(exited) == 2 and all(exited), errors
    run = next(folder.glob("run-*"))
    metadata = json.loads((run / "metadata.json").read_text(encoding="utf-8"))
    assert metadata["run_status"] == "invalid" and "membership changed" in metadata["execution_error"]
    samples = json.loads((run / "samples.json").read_text(encoding="utf-8"))
    assert len(samples) == 2 and samples[0]["status"] == "ok" and samples[1]["status"] == "cleanup-failed"
    with (run / "results.csv").open(newline="", encoding="utf-8") as stream:
        assert all(row["status"] == "cleanup-failed" and not row["median_ms"] for row in csv.DictReader(stream))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.reference = str(Path(args.reference).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        sys.path.insert(0, str(ROOT / "tests" / "lib"))
        from xraytest.windows_owned_job import establish_job
        establish_job()
    command = [sys.executable, str(Path(__file__).with_name("run.py")),
               "--reference", args.reference, "--output", str(args.output),
               "--quick", "--samples", "5", "--cpython"]
    process = subprocess.run(command, capture_output=True, text=True)
    if process.returncode:
        raise AssertionError(process.stdout + process.stderr)
    result_path = Path(process.stdout.split("results: ")[-1].strip())
    with result_path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 6, rows
    assert all(row["status"] == "ok" and row["samples"] == "5" and row["median_ms"] for row in rows)
    metadata = json.loads(result_path.with_name("metadata.json").read_text(encoding="utf-8"))
    assert metadata["quick"] and metadata["measurement"] == "coarse"
    assert all(candidate["binary"]["sha256"] for candidate in metadata["candidates"])
    samples = json.loads(result_path.with_name("samples.json").read_text(encoding="utf-8"))
    for row in rows:
        times = [sample["elapsed_ms"] for sample in samples
                 if (sample["case"], sample["candidate"]) == (row["case"], row["candidate"])]
        assert float(row["median_ms"]) == round(statistics.median(times), 6)

    bad_candidate = args.output / "bad-candidate.json"
    bad_candidate.write_text(json.dumps([
        {"name": "wrong", "command": [sys.executable, "-c", "print(0)"]},
        {"name": "quota", "command": [sys.executable, "-c", "import sys;sys.stderr.write('work limit exceeded');sys.exit(1)"]},
        {"name": "failed", "command": [sys.executable, "-c", "import sys;sys.exit(2)"]},
        {"name": "timeout", "command": [sys.executable, "-c", "import time;time.sleep(2)"]},
    ]), encoding="utf-8")
    process = subprocess.run(command + ["--case", "fib", "--candidates", str(bad_candidate), "--timeout", "1"],
                             capture_output=True, text=True)
    assert process.returncode == 1, process.stdout + process.stderr
    result_path = Path(process.stdout.split("results: ")[-1].strip())
    with result_path.open(newline="", encoding="utf-8") as stream:
        rows = {row["candidate"]: row for row in csv.DictReader(stream)}
    assert rows["wrong"]["status"] == "wrong-output" and not rows["wrong"]["median_ms"]
    assert rows["quota"]["status"] == "quota-failed" and not rows["quota"]["median_ms"]
    assert rows["failed"]["status"] == "failed" and not rows["failed"]["median_ms"]
    assert rows["timeout"]["status"] == "timeout" and not rows["timeout"]["median_ms"]
    assert rows["c"]["status"] == "ok" and rows["cpython"]["status"] == "ok"
    samples = json.loads(result_path.with_name("samples.json").read_text(encoding="utf-8"))
    for sample in samples:
        if sample["candidate"] == "quota":
            assert sample["returncode"] == 1
            assert Path(sample["stderr"]).read_bytes() == b"work limit exceeded"
    runner = runpy.run_path(str(Path(__file__).with_name("run.py")))
    failed = runner["run_sample"]([str(args.output / "missing-executable")], b"", 1,
                                  args.output / "launch-failed")
    assert failed["status"] == "launch-failed" and failed["returncode"] is None
    assert Path(failed["stderr"]).read_bytes()
    identity_change(command, Path(args.reference), args.output)
    relative_command(command, Path(args.reference), args.output)
    descendant_cleanup(runner, args.output)
    late_descendant(runner, Path(args.reference), args.output)
    print("native_bench_runner: checked results, invalid identities, exact paths, and descendant cleanup")


if __name__ == "__main__":
    main()
