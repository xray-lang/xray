"""Capture and compare complete Release CTest receipts on one host.

Both sides are built from exact commits in a detached worktree with the
canonical Release preset, run through the full registered CTest inventory,
and compared test by test. The comparison fails only on a changed build
configuration, a new failure, a missing required gate test, or a failed
identity check. Timing tests are reported separately because their wall
time is information, not a correctness gate.

The nested sanitizer lane is excluded: it builds and runs a second complete
configuration, is owned by the sanitizer job, and recurses into the whole
inventory instead of reporting one Release result.
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

from merge_slice import (cache_fields, cache_options, current_gate_tests, junit_outcomes,
                         registry_names, verify_identity)
from xray_process import run_child

TIMING = re.compile(r"(?:perf|bench|timing)", re.IGNORECASE)
# The sanitizer lane builds and runs a second complete configuration.
EXCLUDED = {"asan_focused"}
# These options only register tests for unavailable capabilities. Older trees
# had no test-specific switch and registered the suites by default. The full CTest
# receipts retain the registration change independently of the build options.
TEST_REGISTRATION_OPTIONS = {"XR_ENABLE_DAP_SESSION_TESTS", "XR_ENABLE_MCP_RUN_TESTS"}
# The shared identity table keys Windows by its canonical runner name; both
# Windows images report the same target triple.
IDENTITY_HOST = {"windows-2022": "windows-2022", "macos-latest": "macos-latest"}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def command(*args, cwd=None):
    return subprocess.check_output([str(arg) for arg in args], cwd=cwd, timeout=60,
                                   stderr=subprocess.STDOUT).decode("utf-8", "replace").strip()


def git(root, *args):
    return command("git", *args, cwd=root)


def clean(source, head):
    actual = git(source, "rev-parse", "HEAD")
    status = git(source, "status", "--porcelain=v1")
    if actual != head or status or git(source, "diff", "HEAD"):
        raise ValueError(f"The source is not clean at {head}")
    return {"head": actual, "status": status, "diffSha256": hashlib.sha256(b"").hexdigest()}


def capture(source, build, folder, head, host, cmake, ctest, jobs, label):
    folder.mkdir()
    git(source, "switch", "--detach", head)
    record = {"head": head, "source": str(source), "build": str(build), "host": host,
              "side": label, "state": "RUNNING", "identity": clean(source, head), "commands": []}
    receipt = folder / "result.json"
    write_json(receipt, record)

    def run(name, argv, timeout, accepted=(0,)):
        argv = [str(arg) for arg in argv]
        stdout, stderr = (folder / f"{label}-{name}.{stream}" for stream in ("stdout", "stderr"))
        row = {"name": f"{label}-{name}", "argv": argv, "stdout": str(stdout), "stderr": str(stderr),
               "childrenDrained": False}
        record["commands"].append(row)
        write_json(receipt, record)
        with stdout.open("xb") as out, stderr.open("xb") as err:
            status = run_child(argv, cwd=source, stdout=out, stderr=err, timeout=timeout)
        row.update(exit=status, childrenDrained=True, stdoutSHA256=digest(stdout), stderrSHA256=digest(stderr))
        write_json(receipt, record)
        if status not in accepted:
            detail = ""
            try:
                detail = " | ".join(stderr.read_text(encoding="utf-8", errors="replace").strip().splitlines()[-12:])
            except OSError:
                pass
            raise RuntimeError(f"{label} {name} returned {status} at {head}"
                               + (f": {detail}" if detail else "")
                               + f". Logs: {stdout} and {stderr}")
        return stdout, status

    run("configure", [cmake, "--preset", "default", "-B", build], 900)
    run("all", [cmake, "--build", build, "--parallel", jobs, "--target", "all", "--", "-k", "0"], 5400)
    run("cli", [cmake, "--build", build, "--parallel", jobs, "--target", "xray"], 600)
    cli = build / ("xray.exe" if os.name == "nt" else "xray")
    cache = build / "CMakeCache.txt"
    if not cli.is_file() or not cache.is_file():
        raise ValueError(f"The Release build at {head} did not produce a CLI and cache")
    cache_hash, cli_hash = digest(cache), digest(cli)
    fields = cache_fields(cache.read_bytes())
    identity, _ = run("identity", [cli, "--version", "--json"], 60)
    target = verify_identity(identity.read_bytes(), head, IDENTITY_HOST[host], "Release")
    registry, _ = run("registry", [ctest, "--test-dir", build, "--show-only=json-v1"], 120)
    junit = folder / "full.xml"
    raw = [ctest, "--test-dir", build, "--output-on-failure", "--no-tests=error",
           "-j", jobs, "--timeout", "900", "-E", "^(asan_focused)$",
           "--output-junit", junit]
    _, raw_exit = run("ctest", raw, 5400, (0, 8))
    outcomes = junit_outcomes(junit.read_bytes())
    registered = registry_names(registry.read_bytes()) - EXCLUDED
    if set(outcomes) != registered:
        raise ValueError(f"Incomplete or inconsistent CTest results for {head}")
    if raw_exit != (8 if "fail" in outcomes.values() else 0):
        raise ValueError(f"CTest exit {raw_exit} disagrees with its JUnit outcomes for {head}")
    record["finalIdentity"] = clean(source, head)
    if digest(cache) != cache_hash or digest(cli) != cli_hash:
        raise ValueError(f"The CMake cache or CLI changed during CTest for {head}")
    record.update(state="PASS" if raw_exit == 0 else "NEEDS_REVIEW", resourcesReleased=True,
                  target=target, buildOptions=cache_options(fields),
                  release={"output": str(junit), "outputSHA256": digest(junit), "rawExit": raw_exit,
                           "registeredCount": len(outcomes)})
    write_json(receipt, record)
    return outcomes, cache_options(fields)


def compare(baseline, candidate, before, after, base_config, candidate_config, host, required):
    regressions = sorted(name for name, state in after.items()
                         if state != "run" and before.get(name) != state)
    timing = sorted(name for name in regressions if TIMING.search(name))
    gating = [name for name in regressions if name not in timing]
    retired = sorted(before.keys() - after.keys())
    required_nonpass = sorted(name for name in required if after.get(name) != "run")
    configuration_changes = {}
    test_registration_changes = {}
    for key in sorted(base_config.keys() | candidate_config.keys()):
        old, new = base_config.get(key), candidate_config.get(key)
        if old == new:
            continue
        change = {"baseline": old, "candidate": new}
        if key in TEST_REGISTRATION_OPTIONS and {old, new} == {None, "OFF"}:
            test_registration_changes[key] = change
        else:
            configuration_changes[key] = change
    same_configuration = not configuration_changes
    state = "PASS" if same_configuration and not (gating or required_nonpass) else "NEEDS_REVIEW"
    result = {
        "state": state,
        "host": host,
        "baseline": baseline,
        "candidate": candidate,
        "sameConfiguration": same_configuration,
        "configurationChanges": configuration_changes,
        "testRegistrationChanges": test_registration_changes,
        "registered": len(after),
        "baselineCounts": dict(Counter(before.values())),
        "candidateCounts": dict(Counter(after.values())),
        "regressions": gating,
        "timingReportOnly": timing,
        "retired": retired,
        "excluded": sorted(EXCLUDED),
        "requiredNonPass": required_nonpass,
    }
    lines = [
        f"host: {host}",
        f"baseline: {baseline}",
        f"candidate: {candidate}",
        f"state: {state}",
        f"registered: {len(after)}",
        "baseline: " + ", ".join(f"{key}={value}" for key, value in sorted(result["baselineCounts"].items())),
        "candidate: " + ", ".join(f"{key}={value}" for key, value in sorted(result["candidateCounts"].items())),
        "new failures: " + (", ".join(gating) if gating else "(none)"),
        "timing report only: " + (", ".join(timing) if timing else "(none)"),
        "retired: " + (", ".join(retired) if retired else "(none)"),
        "excluded: " + ", ".join(sorted(EXCLUDED)),
        "required non-pass: " + (", ".join(required_nonpass) if required_nonpass else "(none)"),
        f"build configuration identical: {'yes' if same_configuration else 'no'}",
    ]
    for label, changes in (("build option", configuration_changes), ("test registration", test_registration_changes)):
        for key, change in changes.items():
            old = change["baseline"] if change["baseline"] is not None else "(absent)"
            new = change["candidate"] if change["candidate"] is not None else "(absent)"
            lines.append(f"{label} {key}: {old} -> {new}")
    return result, "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--host", required=True, choices=("windows-2022", "macos-latest"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4, choices=range(1, 9))
    args = parser.parse_args(argv)
    if not all(re.fullmatch(r"[0-9a-f]{40}", head) for head in (args.base, args.candidate)):
        parser.error("The run requires two exact commit IDs")
    if args.host == "windows-2022" and os.name != "nt":
        parser.error("The Windows comparison requires Windows")
    if args.host == "macos-latest" and platform.system() != "Darwin":
        parser.error("The macOS comparison requires macOS")
    repository = Path(git(Path.cwd(), "rev-parse", "--show-toplevel"))
    if (git(repository, "merge-base", args.base, args.candidate) != args.base
            or git(repository, "rev-parse", args.base + "^{tree}") ==
               git(repository, "rev-parse", args.candidate + "^{tree}")):
        parser.error("The comparison requires a distinct ancestor and source tree")
    cmake, ctest = (shutil.which(tool) for tool in ("cmake", "ctest"))
    if not all((cmake, ctest)):
        parser.error("CMake and CTest must be available")
    required = current_gate_tests(repository, args.candidate, args.host)
    output = args.output.resolve()
    workspace = output.with_name(output.name + "-workspace")
    output.mkdir()
    workspace.mkdir()
    source = workspace / "source"
    git(repository, "worktree", "prune")
    git(repository, "worktree", "add", "--detach", "--force", str(source), args.base)
    try:
        before, base_config = capture(source, workspace / "base-build", output / "base", args.base,
                                      args.host, cmake, ctest, str(args.jobs), "base")
        after, candidate_config = capture(source, workspace / "candidate-build", output / "candidate",
                                          args.candidate, args.host, cmake, ctest, str(args.jobs),
                                          "candidate")
    except Exception as error:
        message = " ".join(str(error).split())
        write_json(output / "comparison.json", {"state": "ERROR", "host": args.host,
                                                "baseline": args.base, "candidate": args.candidate,
                                                "error": message})
        (output / "summary.txt").write_text(f"host: {args.host}\nbaseline: {args.base}\n"
                                            f"candidate: {args.candidate}\nstate: ERROR\nerror: {message}\n",
                                            encoding="utf-8", newline="\n")
        print(f"::error title=Release comparison ({args.host})::{message[:900]}", flush=True)
        return 1
    finally:
        subprocess.run(["git", "-C", str(repository), "worktree", "remove", "--force", str(source)],
                       capture_output=True)
    result, summary = compare(args.base, args.candidate, before, after, base_config, candidate_config,
                              args.host, required)
    result["hostIdentity"] = {"host": platform.node(), "platform": platform.platform(),
                              "machine": platform.machine(), "python": platform.python_version()}
    write_json(output / "comparison.json", result)
    (output / "summary.txt").write_text(summary, encoding="utf-8", newline="\n")
    print(summary, flush=True)
    return 0 if result["state"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
