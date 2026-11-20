"""Capture complete Release CTest receipts on one macOS host."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess

from merge_slice import cache_fields, cache_options, current_gate_tests, junit_outcomes, registry_names, verify_identity
from xray_process import run_child


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")


def command(*args, cwd=None):
    return subprocess.check_output([str(arg) for arg in args], cwd=cwd, timeout=45).decode("utf-8").strip()


def git(root, *args):
    return command("git", *args, cwd=root)


def clean(source, head):
    actual = git(source, "rev-parse", "HEAD")
    status = git(source, "status", "--porcelain=v1")
    if actual != head or status or git(source, "diff", "HEAD"):
        raise ValueError(f"The source is not clean at {head}")
    return {"head": actual, "status": status, "diffSha256": hashlib.sha256(b"").hexdigest()}


def capture(source, build, folder, head, host, cmake, ctest, jobs):
    folder.mkdir()
    git(source, "switch", "--detach", head)
    record = {"head": head, "source": str(source), "build": str(build), "hostIdentity": host["identity"],
              "state": "RUNNING", "resourcesReleased": False, "identity": clean(source, head), "commands": []}
    receipt = folder / "result.json"
    write_json(receipt, record)

    def run(name, argv, timeout, accepted=(0,)):
        argv = [str(arg) for arg in argv]
        stdout, stderr = (folder / f"candidate-{name}.{stream}" for stream in ("stdout", "stderr"))
        row = {"name": "candidate-" + name, "argv": argv, "stdout": str(stdout), "stderr": str(stderr),
               "childrenDrained": False}
        record["commands"].append(row)
        write_json(receipt, record)
        with stdout.open("xb") as out, stderr.open("xb") as err:
            status = run_child(argv, cwd=source, stdout=out, stderr=err, timeout=timeout)
        row.update(exit=status, childrenDrained=True, stdoutSHA256=digest(stdout), stderrSHA256=digest(stderr))
        write_json(receipt, record)
        if status not in accepted:
            raise RuntimeError(f"{name} returned {status} for {head}. Logs: {stdout} and {stderr}")
        return stdout, status

    run("configure", [cmake, "--preset", "default", "-B", build], 600)
    run("all", [cmake, "--build", build, "--parallel", jobs, "--target", "all", "--", "-k0"], 2400)
    run("cli-post-all", [cmake, "--build", build, "--parallel", jobs, "--target", "xray"], 300)
    cli = build / "xray"
    cache = build / "CMakeCache.txt"
    cache_hash, cli_hash = digest(cache), digest(cli)
    fields = cache_fields(cache.read_bytes())
    compiler = Path(fields["CMAKE_C_COMPILER"]).resolve()
    compiler_version = command(compiler, "--version")
    if "clang version" not in compiler_version:
        raise ValueError(f"Expected host Clang in {cache}")
    toolchain = {**host["tools"], "compiler": str(compiler), "compilerVersion": compiler_version,
                 "compilerSHA256": digest(compiler)}
    shutil.copyfile(cache, folder / "CMakeCache.txt")
    record["trees"] = {"release": {"build": str(build), "cli": str(cli), "cache": str(cache),
        "cacheSHA256": cache_hash, "cliSHA256": cli_hash, "toolchainIdentity": json.dumps(toolchain, sort_keys=True)}}
    identity, _ = run("identity", [cli, "--version", "--json"], 30)
    target = verify_identity(identity.read_bytes(), head, "macos-latest", "Release")
    registry, _ = run("registry", [ctest, "--test-dir", build, "--show-only=json-v1"], 60)
    junit = folder / "full.xml"
    _, raw_exit = run("ctest", [ctest, "--test-dir", build, "--output-on-failure", "--no-tests=error",
                               "-j", jobs, "--output-junit", junit], 4000, (0, 8))
    outcomes = junit_outcomes(junit.read_bytes())
    if set(outcomes) != registry_names(registry.read_bytes()) or raw_exit != (8 if "fail" in outcomes.values() else 0):
        raise ValueError(f"Incomplete or inconsistent CTest results for {head}")
    record["finalIdentity"] = clean(source, head)
    if digest(cache) != cache_hash or digest(cli) != cli_hash:
        raise ValueError(f"The CMake cache or CLI changed during CTest for {head}")
    record.update(state="PASS" if raw_exit == 0 else "NEEDS_REVIEW", resourcesReleased=True,
                  hostIdentity=host["identity"], release={"output": str(junit), "outputSHA256": digest(junit),
                  "registry": str(registry), "registrySHA256": digest(registry), "rawExit": raw_exit,
                  "registeredCount": len(outcomes)})
    write_json(receipt, record)
    return outcomes, (target, toolchain, cache_options(fields))


def installation_evidence(build, output):
    captures = list((build / "lir-cli-commands").glob("lir-cli-*/runtime-install.files.json"))
    if len(captures) != 1:
        raise ValueError("Expected exactly one runtime installation capture in the complete run")
    work = captures[0].parent
    output.mkdir()
    for pattern in ("runtime-install.*", "relocated-set_values*", "relocated-push_capacity*", "result.json"):
        for path in work.glob(pattern):
            if path.is_file() and path.suffix in (".json", ".stdout", ".stderr", ".exit", ".c"):
                shutil.copyfile(path, output / path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4, choices=range(1, 9))
    args = parser.parse_args()
    if platform.system() != "Darwin" or not all(re.fullmatch(r"[0-9a-f]{40}", head)
                                                for head in (args.base, args.candidate)):
        parser.error("The run requires macOS and two exact commit IDs")
    repository = Path(git(Path.cwd(), "rev-parse", "--show-toplevel"))
    if (git(repository, "merge-base", args.base, args.candidate) != args.base
            or git(repository, "rev-parse", args.base + "^{tree}") ==
               git(repository, "rev-parse", args.candidate + "^{tree}")):
        parser.error("The comparison requires a distinct ancestor and source tree")
    cmake, ctest, ninja = (shutil.which(tool) for tool in ("cmake", "ctest", "ninja"))
    if not all((cmake, ctest, ninja)):
        parser.error("CMake, CTest and Ninja must be available")
    host = {"identity": json.dumps({"host": platform.node(), "platform": platform.platform(),
                                    "boot": command("sysctl", "-n", "kern.boottime")}, sort_keys=True),
            "tools": {"sdk": command("xcrun", "--show-sdk-path"), "cmake": command(cmake, "--version"),
                      "ctest": command(ctest, "--version"), "ninja": command(ninja, "--version"),
                      "python": platform.python_version()}}
    output = args.output.resolve()
    workspace = output.with_name(output.name + "-workspace")
    output.mkdir()
    workspace.mkdir()
    source = workspace / "source"
    git(repository, "worktree", "add", "--detach", source, args.base)
    baseline, before = capture(source, workspace / "base-build", output / "base", args.base, host,
                               cmake, ctest, str(args.jobs))
    candidate_build = workspace / "candidate-build"
    candidate, after = capture(source, candidate_build, output / "candidate", args.candidate, host,
                               cmake, ctest, str(args.jobs))
    regressions = sorted(name for name, state in candidate.items() if state != "run" and baseline.get(name) != state)
    retired = sorted(baseline.keys() - candidate.keys())
    required = current_gate_tests(repository, args.candidate) | {"test_lir_cli_commands"}
    required_nonpass = sorted(name for name in required if candidate.get(name) != "run")
    if candidate.get("test_lir_cli_commands") == "run":
        installation_evidence(candidate_build, output / "installed-fixtures")
    state = "PASS" if before == after and not (regressions or retired or required_nonpass) else "NEEDS_REVIEW"
    result = {"state": state, "baseline": args.base, "candidate": args.candidate, "hostIdentity": host["identity"],
              "sameConfiguration": before == after, "baselineCounts": Counter(baseline.values()),
              "candidateCounts": Counter(candidate.values()), "regressions": regressions, "retired": retired,
              "requiredNonPass": required_nonpass}
    write_json(output / "comparison.json", result)
    print(json.dumps(result, indent=2), flush=True)
    return 0 if state == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
