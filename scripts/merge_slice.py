#!/usr/bin/env python3
"""Publish an exact reviewed linear candidate after Task339 verification.

The review pins an official CI attempt and its ZIPs. Release evidence
refers to closed runner receipts. SAN approval covers the exact patch.
"""

import argparse
import ast
from datetime import datetime, timedelta
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from urllib.request import Request, urlopen
from zipfile import ZipFile

import xray_progress
from xray_slot import Lease, save_json
from xray_process import RecoveryRequired


HOST_JOBS = {
    "windows-2022": "Native Windows (MSVC x64)",
    "macos-latest": "Native macOS (Ninja Release)",
    "ubuntu-24.04": "Native Linux (ubuntu-24.04)",
    "ubuntu-24.04-arm": "Native Linux (ubuntu-24.04-arm)",
}
HOST_TARGETS = {
    "windows-2022": ("windows-x86_64",),
    "macos-latest": ("macos-arm64", "macos-aarch64", "macos-x86_64"),
    "ubuntu-24.04": ("linux-x86_64",),
    "ubuntu-24.04-arm": ("linux-arm64", "linux-aarch64"),
}
SAN_JOB = "Current gates (ASan + UBSan + paranoid)"
CI_REPO = "xray-lang/xray"
CI_API = f"https://api.github.com/repos/{CI_REPO}/actions"
UPLOAD_STEP = "Run actions/upload-artifact@v4"
UPLOAD_REGISTRATION_DELAY = timedelta(seconds=5)
ARTIFACT_PAGE_SIZE = 100
METADATA_OWNERSHIP_FILES = {"scripts/check_meta_ownership.py", "scripts/meta_ownership_baseline.json"}
CURRENT_CORE = {"smoke", "test_cgen_verify_output", "test_lir_core", "register_vm", "lir_bytecode"}


def git(root: Path, *arguments: str) -> str:
    result = subprocess.run(["git", "-C", str(root), *arguments], capture_output=True,
                            encoding="utf-8", errors="strict")
    if result.returncode:
        raise RuntimeError(f"git {' '.join(arguments)}: {result.stderr.strip()}")
    return result.stdout.strip()


def ancestor(root: Path, older: str, newer: str) -> bool:
    result = subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", older, newer],
                            capture_output=True)
    if result.returncode not in (0, 1):
        raise RuntimeError("Could not check commit ancestry")
    return result.returncode == 0


def git_paths(root: Path, *arguments: str) -> list[str]:
    result = subprocess.run(["git", "-C", str(root), *arguments, "-z"], capture_output=True)
    if result.returncode:
        raise RuntimeError(f"git {' '.join(arguments)}: {result.stderr.decode('utf-8', errors='strict').strip()}")
    return [path for path in result.stdout.decode("utf-8", errors="strict").split("\0") if path]


def clean(root: Path) -> int:
    """Refresh only blobs proven identical to HEAD, without changing file bytes."""
    if git(root, "diff", "--cached", "--name-only") or git(root, "ls-files", "--others", "--exclude-standard"):
        raise RuntimeError("Integration worktree has staged or untracked changes")
    paths = git_paths(root, "diff-files", "--name-only")
    for path in paths:
        if git(root, "hash-object", "--path=" + path, "--", path) != git(root, "rev-parse", "HEAD:" + path):
            raise RuntimeError(f"Integration worktree has changed content: {path}")
    if paths:
        git(root, "add", "--", *paths)
    if git(root, "status", "--porcelain"):
        raise RuntimeError("Integration worktree is not clean")
    return len(paths)


def materialize_checkout(root: Path, changed: list[str]) -> list[dict]:
    """Apply current Git checkout attributes to reviewed changed regular files."""
    clean(root)
    pending = []
    contents = {}
    for path in changed:
        entries = git(root, "--literal-pathspecs", "ls-files", "--stage", "--", path).splitlines()
        if not entries:
            if git(root, "--literal-pathspecs", "ls-tree", "-z", "HEAD", "--", path):
                raise RuntimeError(f"Checkout candidate has no index entry: {path}")
            continue
        if len(entries) != 1:
            raise RuntimeError(f"Checkout requires one stage-zero index entry: {path}")
        mode, blob, stage = entries[0].split("\t", 1)[0].split()
        if stage != "0" or blob != git(root, "rev-parse", "HEAD:" + path):
            raise RuntimeError(f"Checkout index differs from the candidate: {path}")
        if mode not in {"100644", "100755"}:
            continue
        if git(root, "hash-object", "--path=" + path, "--", path) != blob:
            raise RuntimeError(f"Checkout work file has changed content: {path}")
        expected = subprocess.check_output(["git", "-C", str(root), "cat-file", "--filters",
                                            "--path=" + path, "HEAD:" + path])
        actual = (root / path).read_bytes()
        if actual != expected:
            pending.append({"path": path, "beforeSHA256": hashlib.sha256(actual).hexdigest(),
                            "expectedSHA256": hashlib.sha256(expected).hexdigest(),
                            "expectedBytes": len(expected)})
            contents[path] = expected
    if pending:
        for row in pending:
            if hashlib.sha256((root / row["path"]).read_bytes()).hexdigest() != row["beforeSHA256"]:
                raise RuntimeError(f"Checkout work file moved after preflight: {row['path']}")
        for row in pending:
            (root / row["path"]).write_bytes(contents[row["path"]])
        for row in pending:
            actual = (root / row["path"]).read_bytes()
            if len(actual) != row["expectedBytes"] or hashlib.sha256(actual).hexdigest() != row["expectedSHA256"]:
                raise RuntimeError(f"Git checkout did not materialize the expected bytes: {row['path']}")
    clean(root)
    return pending


def evidence_path(value, *, allow_empty: bool = False) -> Path:
    if not isinstance(value, str):
        raise ValueError("Evidence requires an absolute file path")
    path = Path(value)
    if not path.is_absolute() or not path.is_file() or (not allow_empty and not path.stat().st_size):
        raise ValueError(f"Evidence requires an existing, nonempty absolute file: {path}")
    return path


def review_request(args) -> dict:
    review = json.loads(args.review.read_bytes().decode("utf-8-sig", "strict"))
    if review.get("approved") is not True:
        raise ValueError("Review has not approved this slice")
    for key in ("sourceSHA", "reviewedBase"):
        if not re.fullmatch(r"[0-9a-f]{40}", review.get(key, "")):
            raise ValueError(f"Review requires an exact {key}")
    for key in ("author", "reviewer"):
        if not isinstance(review.get(key), str) or not review[key].strip():
            raise ValueError(f"Review requires a named {key}")
    for key in ("reviewEvidence", "l0Log"):
        evidence_path(review.get(key))
    return review


def review_scope(root: Path, base: str, candidate: str, review: dict) -> dict:
    changes = git_paths(root, "diff", "--no-renames", "--name-status", base, candidate)
    core, front, shared, deletions, safety = set(), set(), set(), set(), set()
    for status, path in zip(changes[::2], changes[1::2]):
        if path.startswith(("src/lir/xlir_lower", "src/frontend/", "src/hir/", "src/xir/", "src/module/",
                            "src/os/", "src/io/", "src/app/", "src/toolchain/", "src/api/", "src/stdlib/", "stdlib/")):
            front.add(path)
        elif path.startswith(("src/lir/", "src/vm/", "src/runtime/", "include/xray_native")):
            core.add(path)
        elif path.startswith(("src/base/", "src/shared/", "src/core/", "include/", ".github/workflows/")) or path in (
            "AGENTS.md", "CMakeLists.txt", "scripts/merge_slice.py", "scripts/merge-slice.ps1",
            "scripts/verify_current_build.py", "scripts/xray_slot.py", "scripts/xray-slot.ps1",
            "scripts/xray_process.py", "scripts/xray_progress.py", "tests/tools/test_merge_slice.py",
              *METADATA_OWNERSHIP_FILES,
            "tests/smoke/run_smoke.py", "tests/smoke/smoke.cmake", "tests/smoke/cases.json",
            "tests/smoke/original-baseline.json",
        ):
            shared.add(path)
        if status == "D":
            deletions.add(path)
        if path.startswith(("src/runtime/", "src/vm/", "src/lir/xlir_c", "src/lir/xlir_ownership",
                            "src/lir/xlir_value_", "include/xray_native")) or path == "src/base/xsigned_rc.h":
            safety.add(path)
    reasons = sorted(shared | deletions | safety)
    if core and front:
        reasons.append("cross-lane source")
    independent = review["author"].strip().casefold() != review["reviewer"].strip().casefold()
    if reasons and not independent:
        raise ValueError("Independent review required: " + ", ".join(reasons))
    return {"independent": independent, "required_by": reasons, "affected_san": sorted(safety)}


def archive(value, name: str, logs: Path, log: dict, *, allow_empty: bool = False) -> bytes:
    path = evidence_path(value, allow_empty=allow_empty)
    content = path.read_bytes()
    (logs / name).write_bytes(content)
    log.setdefault("evidence", {})[name] = {
        "source": str(path), "sha256": hashlib.sha256(content).hexdigest(),
    }
    return content


def registry_names(content: bytes) -> set[str]:
    names = [test["name"] for test in json.loads(content)["tests"]]
    if not names or len(set(names)) != len(names):
        raise ValueError("CTest inventory is empty or contains duplicate names")
    return set(names)


def junit_outcomes(content: bytes) -> dict[str, str]:
    results = {}
    states = {"run": (), "notrun": ("skipped",), "fail": ("failure",)}
    document = ET.fromstring(content)
    for case in document.iter("testcase"):
        name, status = case.get("name"), case.get("status")
        tags = tuple(child.tag for child in case if child.tag in {"failure", "error", "skipped"})
        if not name or name in results or status not in states or states[status] != tags:
            raise ValueError("CTest JUnit contains duplicate, incomplete or invalid outcomes")
        results[name] = status
    if not results:
        raise ValueError("CTest JUnit contains no outcomes")
    return results


def current_gate_tests(root: Path, candidate: str) -> set[str]:
    source = git(root, "show", f"{candidate}:scripts/verify_current_build.py")
    for item in ast.parse(source).body:
        if isinstance(item, ast.Assign) and any(isinstance(target, ast.Name) and target.id == "REQUIRED"
                                               for target in item.targets):
            tests = ast.literal_eval(item.value)
            if (isinstance(tests, tuple) and all(isinstance(name, str) for name in tests) and CURRENT_CORE <= set(tests)
                    and len(set(tests)) == len(tests)):
                return set(tests)
    raise ValueError("Candidate has no canonical list of current tests")


def verify_identity(content: bytes, candidate: str, host: str, profile: str) -> str:
    identity = json.loads(content)
    if (identity.get("commit") != candidate or identity.get("dirty") is not False or
            identity.get("target") not in HOST_TARGETS[host] or identity.get("buildProfile") != profile):
        raise ValueError(f"{host}: build identity does not match {candidate} ({profile})")
    return identity["target"].replace("aarch64", "arm64")


def stored_file(name: str, content: bytes, logs: Path, log: dict) -> bytes:
    (logs / name).write_bytes(content)
    log.setdefault("evidence", {})[name] = {"sha256": hashlib.sha256(content).hexdigest()}
    return content


def official_json(endpoint: str, name: str, logs: Path, log: dict) -> dict:
    request = Request(CI_API + endpoint, headers={"User-Agent": "xray-integration-queue",
                                                  "Accept": "application/vnd.github+json"})
    with urlopen(request, timeout=30) as response:
        return json.loads(stored_file(name, response.read(), logs, log))


def native_path(host: str, path: str):
    path_type = PureWindowsPath if host == "windows-2022" else PurePosixPath
    result = path_type(path)
    if not result.is_absolute() or ".." in result.parts:
        raise ValueError(f"{host}: nonabsolute command path: {path}")
    return result


def tool_name(path: str) -> str:
    return PurePosixPath(path.replace("\\", "/")).name.lower().removesuffix(".exe")


def cache_fields(content: bytes) -> dict[str, str]:
    fields = {}
    for line in content.decode("utf-8").splitlines():
        match = re.fullmatch(r"([A-Za-z_][A-Za-z_0-9]*):\w+=(.*)", line)
        if match:
            fields[match[1]] = match[2]
    return fields


def cache_options(fields: dict[str, str]) -> dict[str, str]:
    return {key: value for key, value in fields.items()
            if re.fullmatch(r"(?:CMAKE_(?:BUILD_TYPE|GENERATOR|C_COMPILER|CXX_COMPILER|C_FLAGS.*|CXX_FLAGS.*|"
                            r"OSX_\w+|SYSTEM_NAME|SYSTEM_PROCESSOR)|ENABLE_\w+|XR_\w+|XRAY_\w+|BUILD_TESTS)", key)}


def require_build(cache: dict[str, str], host: str, profile: str):
    if (cache.get("CMAKE_BUILD_TYPE") != profile or cache.get("CMAKE_GENERATOR") != "Ninja" or
            not cache.get("CMAKE_C_COMPILER")):
        raise ValueError(f"{host}: expected an identified {profile} Ninja build")
    return native_path(host, cache["CMAKE_CACHEFILE_DIR"])


def valid_argv(argv) -> bool:
    return isinstance(argv, list) and bool(argv) and all(isinstance(arg, str) and arg for arg in argv)


def verify_current(package: ZipFile, host: str, candidate: str, tests: set[str], logs: Path, log: dict,
                   profile: str = "Release") -> None:
    prefix = host if profile == "Release" else "san"
    directory = "current-evidence" if profile == "Release" else "sanitizer-evidence"

    def read(member: str, destination: str) -> bytes:
        if package.namelist().count(member) != 1:
            raise ValueError(f"{prefix}: missing or duplicate artifact member {member}")
        return stored_file(prefix + "-" + destination, package.read(member), logs, log)

    cache = cache_fields(read("build/CMakeCache.txt", "CMakeCache.txt"))
    build = require_build(cache, host, profile)
    source = native_path(host, cache["CMAKE_HOME_DIRECTORY"])
    if build != source / "build":
        raise ValueError(f"{prefix}: artifact cache is not the workflow build")
    if profile == "Debug" and any(cache.get(option) != "ON"
                                  for option in ("ENABLE_ASAN", "ENABLE_UBSAN", "XR_XIR_PARANOID")):
        raise ValueError("Sanitizer build lacks ASan, UBSan or paranoid mode")

    exe = "xray.exe" if host == "windows-2022" else "xray"
    commands = json.loads(read(f"{directory}/commands.json", "commands.json"))
    expected = {"identity", "ctest-list", "current-tests"}
    if len(commands) != len(expected) or {row["name"] for row in commands} != expected:
        raise ValueError(f"{prefix}: incomplete current gate commands")
    for row in commands:
        name, argv = row["name"], row["command"]
        if row.get("exit") != 0 or not valid_argv(argv):
            raise ValueError(f"{prefix}: {name} did not complete successfully")
        if name == "identity":
            valid = len(argv) == 3 and native_path(host, argv[0]) == build / exe and argv[1:] == ["--version", "--json"]
        else:
            valid = len(argv) >= 4 and tool_name(argv[0]) == "ctest" and argv[1] == "--test-dir" and native_path(host, argv[2]) == build
            if name == "ctest-list":
                valid = valid and argv[3:] == ["--show-only=json-v1"]
            else:
                pattern = argv[8] if len(argv) == 11 else ""
                branches = pattern[2:-2].split("|") if pattern.startswith("^(") and pattern.endswith(")$") else []
                valid = (valid and argv[3:8] == ["--output-on-failure", "--no-tests=error", "-j", "2", "-R"] and
                         len(branches) == len(tests) and set(branches) == {re.escape(test) for test in tests} and
                         argv[9] == "--output-junit" and native_path(host, argv[10]) == source / directory / "current.xml")
        if not valid:
            raise ValueError(f"{prefix}: {name} has unexpected command or build")
        for stream in ("stdout", "stderr"):
            filename = f"{name}.{stream}"
            content = read(f"{directory}/{filename}", filename)
            if hashlib.sha256(content).hexdigest() != row.get(stream + "Sha256"):
                raise ValueError(f"{prefix}: changed {name} {stream}")
            if name == "identity" and stream == "stdout":
                verify_identity(content, candidate, host, profile)
            if name == "ctest-list" and stream == "stdout":
                if not 0 <= row.get("seconds", -1) <= 5 or not tests <= registry_names(content):
                    raise ValueError(f"{prefix}: incomplete or slow CTest registration")
    if junit_outcomes(read(f"{directory}/current.xml", "current.xml")) != dict.fromkeys(tests, "run"):
        raise ValueError(f"{prefix}: current gates did not execute and pass the exact test list")


def release_receipt(paths: dict, host: str, sha: str, expected_host: str, logs: Path, log: dict, side: str):
    prefix = f"{host}-{side}"
    record = json.loads(archive(paths["receipt"], prefix + "-receipt.json", logs, log))
    if (record.get("head") != sha or record.get("resourcesReleased") is not True or
            record.get("state") not in ("PASS", "FAIL", "NEEDS_REVIEW") or record.get("hostIdentity") != expected_host):
        raise ValueError(f"{prefix}: expected a closed, released run on the approved host")
    empty_diff = hashlib.sha256(b"").hexdigest()
    for key in ("identity", "finalIdentity"):
        source_identity = record.get(key) or {}
        if (source_identity.get("head") != sha or source_identity.get("status") != "" or
                source_identity.get("diffSha256") != empty_diff):
            raise ValueError(f"{prefix}: missing or changed clean source identity ({key})")
    files = {key: archive(paths[key], f"{prefix}-{key}", logs, log, allow_empty=key == "stderr")
             for key in ("identity", "registry", "junit", "cache", "log", "stderr")}
    target = verify_identity(files["identity"], sha, host, "Release")
    cache = cache_fields(files["cache"])
    build = require_build(cache, host, "Release")
    source = native_path(host, cache["CMAKE_HOME_DIRECTORY"])
    if native_path(host, record["build"]) != build or native_path(host, record["source"]) != source:
        raise ValueError(f"{prefix}: cache is outside the recorded source or build")
    tree = record["trees"]["release"]
    exe = "xray.exe" if host == "windows-2022" else "xray"
    toolchain = tree.get("toolchainIdentity")
    if (native_path(host, tree["build"]) != build or native_path(host, tree["cli"]) != build / exe or
            native_path(host, tree["cache"]) != build / "CMakeCache.txt" or
            tree.get("cacheSHA256") != hashlib.sha256(files["cache"]).hexdigest() or
            not re.fullmatch(r"[0-9a-f]{64}", tree.get("cliSHA256", "")) or
            not isinstance(toolchain, str) or not toolchain.strip()):
        raise ValueError(f"{prefix}: release tree receipt lacks matching cache, CLI or toolchain identity")
    selected = ("candidate-configure", "candidate-all", "candidate-cli-post-all",
                "candidate-identity", "candidate-registry", "candidate-ctest")
    commands = [row for row in record["commands"] if row.get("name") in selected]
    if [row["name"] for row in commands] != list(selected):
        raise ValueError(f"{prefix}: missing, duplicate or out-of-order Release commands")
    for row in commands:
        if row.get("childrenDrained") is not True or not valid_argv(row.get("argv")):
            raise ValueError(f"{prefix}: unclosed or invalid command")
    if any(row.get("exit") != 0 for row in commands[:-1]):
        raise ValueError(f"{prefix}: Release build, CLI, or inventory failed")
    config, all_build, rebuild, identity, registry, ctest = (row["argv"] for row in commands)
    if (tool_name(config[0]) != "cmake" or config[1:3] != ["--preset", "default"] or
            not (config[3:] == [] and build == source / "build" or
                 len(config) == 5 and config[3] == "-B" and native_path(host, config[4]) == build)):
        raise ValueError(f"{prefix}: Release configure argv is not bound to this build")

    def build_tail(argv):
        if (len(argv) < 6 or tool_name(argv[0]) != "cmake" or argv[1] != "--build" or
                native_path(host, argv[2]) != build or argv[3] != "--parallel" or
                not re.fullmatch(r"[1-9][0-9]*", argv[4])):
            raise ValueError(f"{prefix}: invalid same-tree Release build command")
        return argv[5:]

    all_forms = (["--", "-k0"], ["--", "-k", "0"], ["--target", "all", "--", "-k0"],
                 ["--target", "all", "--", "-k", "0"])
    if build_tail(all_build) not in all_forms or build_tail(rebuild) != ["--target", "xray"]:
        raise ValueError(f"{prefix}: expected all and independent CLI rebuild commands")
    if len(identity) != 3 or native_path(host, identity[0]) != build / exe or identity[1:] != ["--version", "--json"]:
        raise ValueError(f"{prefix}: CLI identity command does not name this build")
    for argv in (registry, ctest):
        if len(argv) < 4 or tool_name(argv[0]) != "ctest" or argv[1] != "--test-dir" or native_path(host, argv[2]) != build:
            raise ValueError(f"{prefix}: CTest command does not name this build")
    if registry[3:] != ["--show-only=json-v1"]:
        raise ValueError(f"{prefix}: CTest inventory is filtered")
    release = record["release"]
    flags = ctest[3:-2]
    if (flags[:2] != ["--output-on-failure", "--no-tests=error"] or
            not re.fullmatch(r"(?:-j(?: )?[1-9][0-9]*|--parallel [1-9][0-9]*)?", " ".join(flags[2:])) or
            ctest[-2] != "--output-junit" or native_path(host, ctest[-1]) != native_path(host, release["output"])):
        raise ValueError(f"{prefix}: full CTest command is filtered or names a different JUnit")
    for row, stream, key in ((commands[3], "stdout", "identity"), (commands[4], "stdout", "registry"),
                             (commands[5], "stdout", "log"), (commands[5], "stderr", "stderr"),
                             (release, "output", "junit"), (release, "registry", "registry")):
        if row[stream + "SHA256"] != hashlib.sha256(files[key]).hexdigest():
            raise ValueError(f"{prefix}: changed or unrelated {key} bytes")
    outcomes = junit_outcomes(files["junit"])
    raw_exit = 8 if "fail" in outcomes.values() else 0
    if (type(commands[-1].get("exit")) is not int or commands[-1]["exit"] != raw_exit or release.get("rawExit") != raw_exit or
            set(outcomes) != registry_names(files["registry"])):
        raise ValueError(f"{prefix}: CTest exit or JUnit does not cover complete inventory")
    return outcomes, (target, toolchain, cache_options(cache))


def verify_release_pair(pair: dict, host: str, candidate: str, baseline: str, tests: set[str],
                        logs: Path, log: dict) -> None:
    # The review identifies the common physical host for these artifacts.
    if not isinstance(pair.get("hostIdentity"), str) or not pair["hostIdentity"].strip():
        raise ValueError(f"{host}: full comparison must identify its host")
    outcomes, configs = zip(*(release_receipt(pair[side], host, sha, pair["hostIdentity"], logs, log, side)
                              for side, sha in (("base", baseline), ("candidate", candidate))))
    if configs[0] != configs[1]:
        raise ValueError(f"{host}: full comparison changes the build configuration")
    before, after = outcomes
    regressions = sorted(name for name, outcome in after.items()
                         if outcome != "run" and before.get(name) != outcome)
    retired = set(before) - set(after)
    approved = pair.get("retired", {})
    if set(approved) != retired or any(not isinstance(reason, str) or not reason.strip() for reason in approved.values()):
        raise ValueError(f"{host}: full comparison changed test registration without reviewed retirement")
    if regressions:
        raise ValueError(f"{host}: new failing or unexecuted tests: " + ", ".join(regressions))
    if not tests <= {name for name, state in after.items() if state == "run"}:
        raise ValueError(f"{host}: complete candidate result misses an applicable safety or smoke test")
    log.setdefault("releaseComparison", {})[host] = {
        "host": pair["hostIdentity"], "base": baseline, "candidate": candidate,
        "registered": len(after), "retired": approved,
    }


def official_run_artifacts(run_id: int, logs: Path, log: dict) -> dict[int, tuple[str, datetime]]:
    """Read every page so earlier attempts and name collisions stay visible."""
    artifacts = {}
    total = None
    page = 1
    while True:
        endpoint = f"/runs/{run_id}/artifacts?per_page={ARTIFACT_PAGE_SIZE}&page={page}"
        listing = official_json(endpoint, f"ci-artifacts-page-{page}.json", logs, log)
        if not isinstance(listing, dict):
            raise ValueError("Invalid official run artifact listing")
        count, rows = listing.get("total_count"), listing.get("artifacts")
        if (type(count) is not int or count < 0 or (total is not None and count != total) or
                not isinstance(rows, list) or len(rows) != min(ARTIFACT_PAGE_SIZE, count - len(artifacts))):
            raise ValueError("Incomplete or inconsistent official run artifact listing")
        total = count
        for row in rows:
            if not isinstance(row, dict):
                raise ValueError("Invalid official run artifact record")
            artifact_id, name = row.get("id"), row.get("name")
            if (type(artifact_id) is not int or artifact_id <= 0 or artifact_id in artifacts or
                    not isinstance(name, str) or not name):
                raise ValueError("Invalid or duplicate official run artifact record")
            artifacts[artifact_id] = (name, parse_time(row.get("created_at")))
        if len(artifacts) == total:
            return artifacts
        page += 1


def verified_ci_package(run: dict, job: dict, artifact: dict, name: str, required_steps: list[str],
                        run_artifacts: dict[int, tuple[str, datetime]],
                        logs: Path, log: dict) -> ZipFile:
    steps = job["steps"]
    for step_name in ["Run actions/checkout@v4", *required_steps, UPLOAD_STEP]:
        matches = [step for step in steps if step.get("name") == step_name]
        if len(matches) != 1 or matches[0].get("status") != "completed" or matches[0].get("conclusion") != "success":
            raise ValueError(f"{job['name']}: missing or failed CI step {step_name}")
    upload = next(step for step in steps if step["name"] == UPLOAD_STEP)
    artifact_id = artifact.get("id")
    if type(artifact_id) is not int or artifact_id <= 0:
        raise ValueError(f"{job['name']}: missing official artifact ID")
    meta = official_json(f"/artifacts/{artifact_id}", name + "-artifact.json", logs, log)
    owner = meta.get("workflow_run") or {}
    repo_id = run["repository"]["id"]
    expected_name = name + "-" + run["head_sha"]
    created = parse_time(meta["created_at"])
    start, end = parse_time(upload["started_at"]), parse_time(upload["completed_at"])
    upper = end + UPLOAD_REGISTRATION_DELAY
    if (meta.get("id") != artifact_id or meta.get("name") != expected_name or meta.get("expired") is not False or
            owner.get("id") != run["id"] or owner.get("head_sha") != run["head_sha"] or owner.get("repository_id") != repo_id or
            owner.get("head_repository_id") != repo_id or end < start or not start <= created <= upper):
        raise ValueError(f"{job['name']}: artifact does not belong to this run and upload step")
    matches = [item_id for item_id, (item_name, date) in run_artifacts.items()
               if item_name == expected_name and start <= date <= upper]
    if matches != [artifact_id] or run_artifacts[artifact_id][1] != created:
        raise ValueError(f"{job['name']}: upload window must contain exactly the requested artifact")
    # Read only the archived bytes whose digest was checked.
    content = archive(artifact["zip"], name + ".zip", logs, log)
    digest = log["evidence"][name + ".zip"]["sha256"]
    if meta.get("digest") != "sha256:" + digest or meta.get("size_in_bytes") != len(content):
        raise ValueError(f"{job['name']}: ZIP differs from the official artifact digest")
    return ZipFile(logs / (name + ".zip"))


def parse_time(value: str) -> datetime:
    if not isinstance(value, str):
        raise ValueError("Official timestamp must be a string")
    timestamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if timestamp.utcoffset() is None:
        raise ValueError("Official timestamp must include a time zone")
    return timestamp


def verify_metadata_ownership(root: Path, changed: list[str], logs: Path, log: dict) -> None:
    if not any(path in METADATA_OWNERSHIP_FILES or re.match(r"src/(lir|hir|xir|module|vm/register)/", path)
               for path in changed):
        return
    name = "meta_ownership_inventory"
    log["gates"][name] = "FAIL"
    command = [sys.executable, "-B", str(root / "scripts/check_meta_ownership.py"),
               "--root", str(root), "--baseline", str(root / "scripts/meta_ownership_baseline.json"),
               "--max-category", "AST_PTR_INTO_IR=0", "--max-category", "PTR_ACROSS_GROWTH=0",
               "--max-category", "CGEN_BORROWED_NAME=0"]
    terminal = subprocess.run(command, cwd=root, capture_output=True, timeout=30)
    stored_file(name + ".stdout", terminal.stdout, logs, log)
    stored_file(name + ".stderr", terminal.stderr, logs, log)
    if terminal.returncode != 0:
        raise ValueError(f"{name} exited with status {terminal.returncode}")
    log["gates"][name] = "PASS"


def verify_task339(root: Path, candidate: str, review: dict, logs: Path, log: dict) -> None:
    for key in ("reviewEvidence", "l0Log"):
        archive(review[key], key + ".log", logs, log)
    ci = review["ci"]
    run_id, attempt = ci.get("runId"), ci.get("runAttempt")
    if type(run_id) is not int or type(attempt) is not int or min(run_id, attempt) <= 0:
        raise ValueError("CI evidence requires a positive official run and attempt")
    endpoint = f"/runs/{run_id}/attempts/{attempt}"
    run = official_json(endpoint, "ci-run.json", logs, log)
    if (run.get("id") != run_id or run.get("run_attempt") != attempt or run.get("head_sha") != candidate or
            run.get("status") != "completed" or run.get("conclusion") != "success" or
            run.get("path") != ".github/workflows/native-host-portability.yml" or
            run.get("repository", {}).get("full_name") != CI_REPO or run.get("head_repository", {}).get("full_name") != CI_REPO):
        raise ValueError("CI run does not bind a successful official portability attempt to the candidate")
    jobs = official_json(endpoint + "/jobs?per_page=100", "ci-jobs.json", logs, log)["jobs"]

    def official_job(name):
        matches = [job for job in jobs if job.get("name") == name and job.get("run_id") == run_id and
                   job.get("run_attempt") == attempt and job.get("head_sha") == candidate and
                   job.get("run_url") == f"{CI_API}/runs/{run_id}"]
        if len(matches) != 1 or matches[0].get("status") != "completed" or matches[0].get("conclusion") != "success":
            raise ValueError("Missing or failed CI job: " + name)
        return matches[0]

    run_artifacts = official_run_artifacts(run_id, logs, log)
    required = current_gate_tests(root, candidate)
    for host, title in HOST_JOBS.items():
        kind = "Windows" if host == "windows-2022" else "Unix"
        steps = [f"Configure {kind} Ninja Release", f"Build {kind} all without exclusions",
                 f"Independently rebuild {kind} CLI", f"Verify {kind} current gates"]
        with verified_ci_package(run, official_job(title), ci["artifacts"][host], "current-" + host,
                                 steps, run_artifacts, logs, log) as package:
            verify_current(package, host, candidate, required, logs, log)
    log["gates"]["L1"] = "PASS"

    steps = ["Configure combined sanitizers", "Build current products and kernels", "Verify exact CLI and current gates"]
    with verified_ci_package(run, official_job(SAN_JOB), ci["artifacts"]["san"], "current-sanitizers",
                             steps, run_artifacts, logs, log) as package:
        verify_current(package, "ubuntu-24.04", candidate, required, logs, log, "Debug")
    log["gates"]["combinedSan"] = "PASS"

    paths = log["reviewScope"]["affected_san"]
    if paths:
        log["gates"]["affectedSan"] = "FAIL"
        if "san" not in review:
            raise ValueError("Missing affected sanitizer evidence")
        proof = json.loads(archive(review["san"], "san-scope.json", logs, log))
        if (not log["reviewScope"]["independent"] or proof.get("approved") is not True or
                proof.get("reviewer") != review["reviewer"] or proof.get("candidate") != candidate or
                proof.get("base") != review["reviewedBase"] or proof.get("paths") != paths or
                not isinstance(proof.get("tests"), list) or sorted(proof["tests"]) != sorted(required) or
                not isinstance(proof.get("coverage"), str) or not proof["coverage"].strip()):
            raise ValueError("Affected sanitizer coverage must be independently approved for the exact scope and combined suite")
        log["gates"]["affectedSan"] = "PASS"


def integrate(args, review: dict, log: dict, logs: Path) -> None:
    root, integration = args.repo.resolve(), args.integration.resolve()
    main = git(root, "rev-parse", "refs/heads/main")
    source = git(root, "rev-parse", "--verify", args.branch + "^{commit}")
    log.update(oldMain=main, sourceSHA=source)
    if source != review["sourceSHA"]:
        raise RuntimeError("Branch moved after the recorded review")
    base = review["reviewedBase"]
    if base != main or not ancestor(root, base, source):
        raise RuntimeError("Freeze and review the candidate on the current main before intake")
    log["reviewScope"] = review_scope(root, base, source, review)
    if not log["reviewScope"]["affected_san"]:
        log["gates"]["affectedSan"] = "NOT_APPLICABLE"
    commits = git(root, "rev-list", "--reverse", base + ".." + source).splitlines()
    if not commits or git(root, "rev-list", "--merges", base + ".." + source):
        raise RuntimeError("The queue requires a nonempty, linear slice")
    authors = []
    for commit in commits:
        author, committer = git(root, "show", "-s", "--format=%ae%n%ce", commit).splitlines()
        if any(email.lower().endswith(".invalid") for email in (author, committer)):
            raise RuntimeError("Placeholder author or committer identity is not allowed")
        authors.append(author)
    log["authors"] = sorted(set(authors))
    if git(root, "rev-parse", "--path-format=absolute", "--git-common-dir") != git(
            integration, "rev-parse", "--path-format=absolute", "--git-common-dir"):
        raise RuntimeError("Integration worktree belongs to another repository")
    if git(integration, "rev-parse", "--abbrev-ref", "HEAD") != "HEAD":
        raise RuntimeError("Integration worktree must have a detached HEAD")
    clean(integration)
    git(integration, "switch", "--detach", source)
    try:
        git(integration, "rebase", "--onto", main, base)
    except Exception:
        for marker in ("rebase-merge", "rebase-apply"):
            if Path(git(integration, "rev-parse", "--git-path", marker)).exists():
                git(integration, "rebase", "--abort")
                break
        raise
    candidate = git(integration, "rev-parse", "HEAD")
    log["candidate"] = candidate
    if not ancestor(integration, main, candidate):
        raise RuntimeError("Candidate would not fast-forward main")
    final_committers = []
    for commit in git(integration, "rev-list", main + ".." + candidate).splitlines():
        identities = git(integration, "show", "-s", "--format=%ae%n%ce", commit).splitlines()
        if any(email.lower().endswith(".invalid") for email in identities):
            raise RuntimeError("Rebased candidate has a placeholder author or committer")
        final_committers.append(identities[1])
    log["committers"] = sorted(set(final_committers))
    changed = git_paths(integration, "diff", "--no-renames", "--name-only", main, candidate)
    log["changed"] = changed
    if candidate != source:
        raise RuntimeError("Rebase changed the reviewed candidate; fresh exact evidence is required")
    log["checkoutMaterialization"] = materialize_checkout(integration, changed)
    xray_progress.smoke_ratchet(integration, main, candidate)
    verify_task339(integration, candidate, review, logs, log)
    verify_metadata_ownership(integration, changed, logs, log)
    if git(integration, "rev-parse", "HEAD") != candidate or git(root, "rev-parse", args.branch) != source:
        raise RuntimeError("Candidate or reviewed branch moved while gates were running")
    clean(integration)
    if not ancestor(integration, main, candidate):
        raise RuntimeError("Candidate ancestry changed")
    log["state"] = "VALIDATED"
    save_json(logs / "result.json", log)
    git(root, "update-ref", "refs/heads/main", candidate, main)
    log["state"] = "MERGED"
    log["newMain"] = candidate


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--integration", type=Path, default=Path(r"E:\xray\wt\int"))
    parser.add_argument("--queue", type=Path, default=Path(r"D:\xray-build\queue"))
    parser.add_argument("--docs", type=Path, required=True, help="Current Task339 documentation checkout")
    args = parser.parse_args()
    for name in ("repo", "integration", "queue", "review", "docs"):
        setattr(args, name, getattr(args, name).resolve())
    if not (args.docs / "tasks" / "339-direct-cutover-to-target-architecture-cn.md").is_file():
        parser.error("--docs must identify the current Task339 documentation checkout")
    review = review_request(args)
    started = time.time()
    with Lease(args.queue, "queue", capacity=1, payload={"branch": args.branch, "review": review}) as lease:
        logs = args.queue.resolve() / "logs" / f"{lease.record['sequence']:020d}"
        logs.mkdir(parents=True)
        log = {"schema": 1, "sequence": lease.record["sequence"], "branch": args.branch,
               "review": review, "started": started, "processingStarted": time.time(),
               "state": "RUNNING", "gates": {"L1": "NOT_RUN", "combinedSan": "NOT_RUN", "affectedSan": "NOT_RUN"}}
        save_json(logs / "result.json", log)
        try:
            integrate(args, review, log, logs)
        except RecoveryRequired as error:
            log.update(state="NEEDS_RECOVERY", error=str(error))
            raise
        except Exception as error:
            log.update(state="REJECTED", error=str(error))
        finally:
            log.update(finished=time.time(), elapsedSeconds=time.time() - started)
            save_json(logs / "result.json", log)
            with (args.queue / "results.jsonl").open("a", encoding="utf-8") as summary:
                summary.write(json.dumps(log, ensure_ascii=False) + "\n")
        print(f"{log['state']}: {logs / 'result.json'}", flush=True)
        return 0 if log["state"] == "MERGED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
