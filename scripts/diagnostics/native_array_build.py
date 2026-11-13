#!/usr/bin/env python3
"""Preserve one Linux SAN CLI's arrays build phases and original failures."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


TIMEOUT = 30
LIBRARY_NAMES = (
    "libxray_native_runtime.a", "libxray_xir_scalar.a",
    "libxray_xir_admission.a", "libxray_xir_declarations.a",
    "libxray_compile_resources.a",
)
PUBLIC_HEADERS = (
    "xray_native_runtime.h", "xray_native_state.h", "xray_native_array.h",
    "xray_native_view.h", "xray_native_pod.h", "xray_native_string.h",
    "xray_native_callable.h", "xray_string_cursor.h", "xray_export.h",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def c_string(text: str) -> str:
    return json.loads('"' + text + '"')


def macro_string(header: str, name: str) -> str:
    match = re.search(r'^#define ' + re.escape(name) + r' "((?:\\.|[^"\\])*)"$',
                      header, re.MULTILINE)
    if match is None:
        raise ValueError("missing string macro " + name)
    return c_string(match.group(1))


def linux_process(pid: int) -> dict | None:
    try:
        text = (Path("/proc") / str(pid) / "stat").read_text()
    except (FileNotFoundError, ProcessLookupError):
        return None
    fields = text[text.rfind(")") + 2:].split()
    return {"pid": pid, "state": fields[0], "parent": int(fields[1]),
            "start_ticks": int(fields[19])}


def drain_owned_children() -> dict:
    """Catch compiler groups orphaned when a timed-out CLI is terminated."""
    started = time.monotonic()
    fact = {"complete": False, "observed": [], "reaped": [], "seconds": None}
    observed = set()
    while time.monotonic() - started < 5:
        processes = {}
        for entry in Path("/proc").iterdir():
            if entry.name.isdigit():
                item = linux_process(int(entry.name))
                if item is not None:
                    processes[item["pid"]] = item
        descendants = {os.getpid()}
        while True:
            children = {pid for pid, item in processes.items() if item["parent"] in descendants}
            if children <= descendants:
                break
            descendants.update(children)
        descendants.discard(os.getpid())
        for pid in sorted(descendants):
            item = processes[pid]
            key = (pid, item["start_ticks"])
            if key not in observed:
                observed.add(key)
                fact["observed"].append(item)
            current = linux_process(pid)
            if current is not None and current["start_ticks"] == item["start_ticks"]:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        while True:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                break
            if not pid:
                break
            fact["reaped"].append({"pid": pid, "wait_status": status})
        if not descendants:
            fact["complete"] = True
            break
        time.sleep(0.01)
    fact["seconds"] = time.monotonic() - started
    return fact


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--expected-head", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("this diagnostic records the Linux ELF/SAN path")
    if re.fullmatch(r"[0-9a-f]{40}", args.expected_head) is None:
        parser.error("--expected-head requires the complete commit")

    root, build, cli = (path.resolve(strict=True) for path in
                        (args.root, args.build, args.cli))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {"state": "RUNNING", "expected_head": args.expected_head,
              "root": str(root), "build": str(build), "cli": str(cli),
              "timeout_seconds_per_command": TIMEOUT, "commands": [],
              "blocked": [], "errors": [], "scope": "diagnosis_only",
              "script_sha256": sha256(Path(__file__).resolve()),
              "process_cleanup": "scripts/xray_process.py::run_child",
              "sampling": "one pass, all failures retained"}

    def save() -> None:
        (output / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    save()
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
            raise OSError(ctypes.get_errno(), "cannot own orphaned compiler descendants")
        result["linux_child_subreaper"] = True
        process_path = root / "scripts/xray_process.py"
        spec = importlib.util.spec_from_file_location("diagnostic_xray_process", process_path)
        if spec is None or spec.loader is None:
            raise RuntimeError("cannot load the current owned process runner")
        process = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(process)
        result["process_runner_sha256"] = sha256(process_path)
        environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
        result["sanitizer_environment"] = {
            name: environment.get(name) for name in ("ASAN_OPTIONS", "UBSAN_OPTIONS")}

        def run(name: str, argv: list[str | Path], *, golden: bytes | None = None,
                quiet: bool = False, cwd: Path = root,
                env: dict[str, str] = environment) -> dict:
            command = [str(argument) for argument in argv]
            stdout, stderr = output / (name + ".stdout"), output / (name + ".stderr")
            row = {"name": name, "argv": command, "cwd": str(cwd), "exit": None,
                   "timed_out": False, "launch_error": None, "seconds": None}
            started = time.monotonic()
            with stdout.open("wb") as out, stderr.open("wb") as err:
                try:
                    row["exit"] = process.run_child(
                        command, cwd=str(cwd), env=env, stdin=subprocess.DEVNULL,
                        stdout=out, stderr=err, timeout=TIMEOUT)
                except subprocess.TimeoutExpired:
                    row["timed_out"] = True
                except OSError as error:
                    row["launch_error"] = str(error)
                    err.write((str(error) + "\n").encode("utf-8"))
            row["seconds"] = time.monotonic() - started
            row["drain"] = drain_owned_children()
            row["stdout_sha256"], row["stderr_sha256"] = sha256(stdout), sha256(stderr)
            row["stdout_bytes"], row["stderr_bytes"] = stdout.stat().st_size, stderr.stat().st_size
            row["okay"] = row["exit"] == 0 and not row["timed_out"] and not row["launch_error"]
            row["okay"] = row["okay"] and row["drain"]["complete"] and not row["drain"]["observed"]
            if golden is not None:
                row["golden_equal"] = stdout.read_bytes() == golden
                row["okay"] = row["okay"] and row["golden_equal"] and not row["stderr_bytes"]
            if quiet:
                row["okay"] = row["okay"] and not row["stdout_bytes"] and not row["stderr_bytes"]
            result["commands"].append(row)
            save()
            print(json.dumps(row, sort_keys=True), flush=True)
            if not row["drain"]["complete"]:
                raise RuntimeError("owned process cleanup remains incomplete")
            return row

        header_path = build / "generated/xray_native_link_paths.h"
        header = header_path.read_text(encoding="utf-8")
        compiler = Path(macro_string(header, "XRT_NATIVE_BUILD_COMPILER"))
        built_cli = Path(macro_string(header, "XRT_NATIVE_BUILD_EXECUTABLE"))
        if built_cli.resolve(strict=True) != cli:
            raise ValueError("CLI differs from the recorded native development executable")
        if (cli.parent.parent / "lib/xray/native").exists():
            raise ValueError("an installed native prefix takes precedence over this development directory")
        match = re.search(r'xrt_native_development_libraries\[\]\s*=\s*\{(.*?)\};', header, re.DOTALL)
        if match is None:
            raise ValueError("missing the five-archive native directory")
        libraries = [Path(c_string(value)) for value in
                     re.findall(r'"((?:\\.|[^"\\])*)"', match.group(1))]
        if tuple(path.name for path in libraries) != LIBRARY_NAMES:
            raise ValueError("unexpected archive order or membership")
        sanitizer_flags = []
        for name, flag in (("XRT_NATIVE_ASAN", "-fsanitize=address"),
                           ("XRT_NATIVE_UBSAN", "-fsanitize=undefined")):
            if re.search(r'^#define ' + name + r' 1$', header, re.MULTILINE):
                sanitizer_flags.append(flag)
        if len(sanitizer_flags) != 2:
            raise ValueError("this diagnostic requires the current combined ASan/UBSan product")

        source = root / "demos/02-collections/arrays.xr"
        golden_path = root / "tests/smoke/expected/arrays.txt"
        golden = golden_path.read_bytes()
        if len(golden) != 505:
            raise ValueError("the requested arrays golden differs from the original 505-byte oracle")
        tracked_inputs = [cli, header_path, build / "CMakeCache.txt", source, golden_path,
                          root / "src/app/cli/xcli_lir_native.c", process_path,
                          *libraries, *(root / "include" / name for name in PUBLIC_HEADERS)]
        before = {str(path): sha256(path) for path in tracked_inputs}
        result["inputs_before"] = before
        result["libraries"] = [str(path) for path in libraries]
        result["sanitizer_flags"] = sanitizer_flags
        result["golden_bytes"] = len(golden)
        result["cmake_facts"] = [line for line in (build / "CMakeCache.txt").read_text(
            encoding="utf-8").splitlines() if re.match(
                r'^(CMAKE_C_COMPILER|CMAKE_BUILD_TYPE|ENABLE_ASAN|ENABLE_UBSAN|ENABLE_SANITIZERS|XR_XIR_PARANOID):', line)]
        save()

        for phase in ("head", "porcelain"):
            options = ["rev-parse", "HEAD"] if phase == "head" else ["status", "--porcelain=v1", "--untracked-files=all"]
            run("git-before-" + phase, ["git", "-C", root, *options])

        identity = run("identity", [cli, "--version", "--json"])
        if not identity["okay"]:
            raise ValueError("the fixed CLI identity command failed")
        identity_value = json.loads((output / "identity.stdout").read_text(encoding="utf-8"))
        result["identity"] = identity_value
        if identity_value.get("commit") != args.expected_head or identity_value.get("dirty") is not False:
            raise ValueError("the CLI is not the requested exact clean candidate")

        drivers = {"default": Path("/usr/bin/cc"), "clang": compiler}
        for name, path in drivers.items():
            run(name + "-readlink", ["/usr/bin/readlink", "-f", path])
            run(name + "-version", [path, "--version"])
        result["driver_paths"] = {name: {"requested": str(path), "resolved": str(path.resolve())}
                                  for name, path in drivers.items()}

        run("arrays-check", [cli, "check", source], quiet=True)
        run("arrays-vm-run", [cli, "run", source], golden=golden)
        generated = output / "program.c"
        emitted = run("arrays-c-only", [cli, "build", source, "--c-only", "-o", generated],
                      golden=("Generated: " + str(generated) + "\n").encode("utf-8"))
        for name, extra in (("default", []), ("clang", ["--cc", str(compiler)])):
            executable = output / ("public-" + name)
            compiled = run("public-" + name + "-build", [cli, "build", source, *extra, "-o", executable],
                           golden=("Built: " + str(executable) + "\n").encode("utf-8"))
            if compiled["okay"] and executable.is_file():
                run("public-" + name + "-run", [executable], golden=golden)
            else:
                result["blocked"].append({"name": "public-" + name + "-run", "reason": "build failed or produced no executable"})

        if emitted["okay"] and generated.is_file() and generated.stat().st_size:
            result["generated_c_sha256"] = sha256(generated)
            for name, path in drivers.items():
                directory = output / ("manual-" + name)
                directory.mkdir()
                driver = path.resolve()
                compiler_environment = {"PATH": str(driver.parent) + ":/usr/bin:/bin",
                                        "TMPDIR": os.environ.get("TMPDIR") or "/tmp"}
                object_path, executable = directory / "program.obj", directory / "program.exe"
                compile_argv = [driver, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                "-O2", "-ffp-contract=off", "-c", "-I" + str(root / "include"),
                                generated, "-o", object_path, *sanitizer_flags]
                link_argv = [driver, object_path, "-o", executable, "-Wl,--gc-sections",
                             "-Wl,--start-group", *libraries, "-Wl,--end-group", "-lm", "-pthread",
                             *sanitizer_flags]
                compiled = run("manual-" + name + "-compile", compile_argv, quiet=True,
                               cwd=directory, env=compiler_environment)
                if compiled["okay"] and object_path.is_file():
                    linked = run("manual-" + name + "-link", link_argv, quiet=True,
                                 cwd=directory, env=compiler_environment)
                    if linked["okay"] and executable.is_file():
                        run("manual-" + name + "-run", [executable], golden=golden)
                    else:
                        result["blocked"].append({"name": "manual-" + name + "-run", "reason": "link failed or produced no executable"})
                else:
                    result["blocked"].append({"name": "manual-" + name + "-link/run", "reason": "compile failed or produced no object"})
        else:
            result["blocked"].append({"name": "manual phases", "reason": "C-only failed or produced no C source"})

        for phase in ("head", "porcelain"):
            options = ["rev-parse", "HEAD"] if phase == "head" else ["status", "--porcelain=v1", "--untracked-files=all"]
            run("git-after-" + phase, ["git", "-C", root, *options])
        result["git_unchanged"] = all(
            (output / ("git-before-" + phase + ".stdout")).read_bytes() ==
            (output / ("git-after-" + phase + ".stdout")).read_bytes()
            for phase in ("head", "porcelain"))
        if not result["git_unchanged"]:
            result["errors"].append("repository HEAD or porcelain changed during execution")
        if (output / "git-before-head.stdout").read_text().strip() != args.expected_head:
            result["errors"].append("repository HEAD differs from the requested product commit")
        result["inputs_after"] = {str(path): sha256(path) for path in tracked_inputs}
        result["inputs_unchanged"] = before == result["inputs_after"]
        if not result["inputs_unchanged"]:
            result["errors"].append("fixed diagnostic inputs changed during execution")
    except Exception as error:
        result["errors"].append(type(error).__name__ + ": " + str(error))
    okay = (not result["errors"] and not result["blocked"] and
            bool(result["commands"]) and all(row["okay"] for row in result["commands"]))
    result["state"] = "PASS_DIAGNOSTIC" if okay else "FAIL_DIAGNOSTIC"
    save()
    return 0 if okay else 1


if __name__ == "__main__":
    raise SystemExit(main())
