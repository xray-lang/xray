#!/usr/bin/env python3
"""Publish an exact reviewed linear candidate after Task339 verification.

The approved review names absolute raw evidence files. ciRun/ciJobs are the
GitHub Actions run and jobs responses. current maps the four runner labels to
their uploaded current-evidence directories. release maps windows-2022 and
macos-latest to hostIdentity and base/candidate records with identity,
registry, junit, cache, log paths and actual CTest exit codes. comparisonBase
pins the approved full-test baseline. san selects the combined CI suite
(current, cache and affected tests) where required. Records compare complete
JUnit outcomes; separately replayed timeouts need explicit qualification.
"""

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

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
    review = json.loads(args.review.read_text(encoding="utf-8-sig"))
    if review.get("approved") is not True:
        raise ValueError("Review has not approved this slice")
    for key in ("sourceSHA", "reviewedBase", "comparisonBase"):
        if not re.fullmatch(r"[0-9a-f]{40}", review.get(key, "")):
            raise ValueError(f"Review requires an exact {key}")
    for key in ("author", "reviewer"):
        if not isinstance(review.get(key), str) or not review[key].strip():
            raise ValueError(f"Review requires a named {key}")
    for key in ("reviewEvidence", "l0Log", "ciRun", "ciJobs"):
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
            "scripts/verify_current_build.py", "tests/smoke/run_smoke.py", "tests/smoke/smoke.cmake",
        ):
            shared.add(path)
        if status == "D":
            deletions.add(path)
        if path.startswith(("src/runtime/", "src/vm/", "src/lir/xlir_c", "src/lir/xlir_ownership",
                            "src/lir/xlir_value_", "include/xray_native")):
            safety.add(path)
    reasons = sorted(shared | deletions | safety)
    if core and front:
        reasons.append("cross-lane source")
    independent = review["author"].strip().casefold() != review["reviewer"].strip().casefold()
    if reasons and not independent:
        raise ValueError("Independent review required: " + ", ".join(reasons))
    return {"independent": independent, "required_by": reasons, "affected_san": bool(safety)}


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
            if (isinstance(tests, tuple) and "smoke" in tests and all(isinstance(name, str) for name in tests)
                    and len(set(tests)) == len(tests)):
                return set(tests)
    raise ValueError("Candidate has no canonical list of current tests")


def verify_identity(content: bytes, candidate: str, host: str, profile: str) -> None:
    identity = json.loads(content)
    if (identity.get("commit") != candidate or identity.get("dirty") is not False or
            identity.get("target") not in HOST_TARGETS[host] or identity.get("buildProfile") != profile):
        raise ValueError(f"{host}: build identity does not match {candidate} ({profile})")


def verify_current(directory: str, host: str, candidate: str, tests: set[str], logs: Path, log: dict,
                   profile: str = "Release") -> None:
    root = Path(directory)
    prefix = host if profile == "Release" else "san"
    commands = json.loads(archive(str(root / "commands.json"), prefix + "-commands.json", logs, log))
    expected = {"identity", "ctest-list", "current-tests"}
    if len(commands) != len(expected) or {row["name"] for row in commands} != expected:
        raise ValueError(f"{prefix}: incomplete current gate commands")
    for row in commands:
        name = row["name"]
        if row.get("exit") != 0 or not isinstance(row.get("command"), list) or not row["command"]:
            raise ValueError(f"{prefix}: {name} did not complete successfully")
        for stream in ("stdout", "stderr"):
            source = root / f"{name}.{stream}"
            content = archive(str(source), prefix + "-" + source.name, logs, log, allow_empty=stream == "stderr")
            if hashlib.sha256(content).hexdigest() != row.get(stream + "Sha256"):
                raise ValueError(f"{prefix}: changed {name} {stream}")
            if name == "identity" and stream == "stdout":
                verify_identity(content, candidate, host, profile)
            if name == "ctest-list" and stream == "stdout":
                if not 0 <= row.get("seconds", -1) <= 5 or not tests <= registry_names(content):
                    raise ValueError(f"{prefix}: incomplete or slow CTest registration")
    junit = archive(str(root / "current.xml"), prefix + "-current.xml", logs, log)
    if junit_outcomes(junit) != dict.fromkeys(tests, "run"):
        raise ValueError(f"{prefix}: current gates did not execute and pass the exact test list")


def cache_options(content: bytes) -> dict[str, str]:
    options = {}
    for line in content.decode("utf-8").splitlines():
        key, marker, value = line.partition("=")
        if marker and re.fullmatch(r"(?:CMAKE_(?:BUILD_TYPE|GENERATOR|C_COMPILER|CXX_COMPILER|C_FLAGS.*|CXX_FLAGS.*)|ENABLE_\w+|XR_\w+):\w+", key):
            options[key.split(":", 1)[0]] = value
    return options


def verify_release_pair(pair: dict, host: str, candidate: str, baseline: str, tests: set[str],
                        logs: Path, log: dict) -> None:
    # The review identifies the common physical host for these artifacts.
    if not isinstance(pair.get("hostIdentity"), str) or not pair["hostIdentity"].strip():
        raise ValueError(f"{host}: full comparison must identify its host")
    outcomes, configs = [], []
    for side, sha in (("base", baseline), ("candidate", candidate)):
        paths = pair[side]
        if type(paths.get("exit")) is not int or paths["exit"] not in (0, 8):
            raise ValueError(f"{host}: full CTest did not finish")
        files = {key: archive(paths[key], f"{host}-{side}-{key}", logs, log)
                 for key in ("identity", "registry", "junit", "cache", "log")}
        verify_identity(files["identity"], sha, host, "Release")
        config = cache_options(files["cache"])
        if config.get("CMAKE_GENERATOR") != "Ninja" or config.get("CMAKE_BUILD_TYPE") != "Release" or not config.get("CMAKE_C_COMPILER"):
            raise ValueError(f"{host}: expected a Release Ninja build with compiler identity")
        configs.append(config)
        result = junit_outcomes(files["junit"])
        if set(result) != registry_names(files["registry"]):
            raise ValueError(f"{host}: CTest output does not cover complete inventory")
        if paths["exit"] != (8 if "fail" in result.values() else 0):
            raise ValueError(f"{host}: CTest exit disagrees with JUnit")
        outcomes.append(result)
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


def verify_task339(root: Path, candidate: str, changed: list[str], review: dict, logs: Path, log: dict) -> None:
    for key in ("reviewEvidence", "l0Log"):
        archive(review[key], key + ".log", logs, log)
    run = json.loads(archive(review["ciRun"], "ci-run.json", logs, log))
    jobs = json.loads(archive(review["ciJobs"], "ci-jobs.json", logs, log))["jobs"]
    if (run.get("head_sha") != candidate or run.get("status") != "completed" or run.get("conclusion") != "success" or
            run.get("path") != ".github/workflows/native-host-portability.yml"):
        raise ValueError("CI run does not bind a successful portability run to the candidate")
    required = current_gate_tests(root, candidate)
    needed_jobs = list(HOST_JOBS.values())
    if log["reviewScope"]["affected_san"] or "san" in review:
        needed_jobs.append(SAN_JOB)
    for name in needed_jobs:
        matches = [job for job in jobs if job.get("name") == name and job.get("run_id") == run.get("id")
                   and job.get("head_sha") == candidate]
        if len(matches) != 1 or matches[0].get("status") != "completed" or matches[0].get("conclusion") != "success":
            raise ValueError("Missing or failed CI job: " + name)
    for host in HOST_JOBS:
        verify_current(review["current"][host], host, candidate, required, logs, log)
    log["gates"]["L1"] = "PASS"

    baseline = review["comparisonBase"]
    if not ancestor(root, baseline, candidate):
        raise ValueError("The approved comparison baseline is not in candidate history")
    win_tests = required.copy()
    if any(re.match(r"src/(lir|hir|xir|module|vm/register)/", path) for path in changed):
        win_tests.add("meta_ownership_inventory")
    for host, tests in (("windows-2022", win_tests), ("macos-latest", required)):
        verify_release_pair(review["release"][host], host, candidate, baseline, tests, logs, log)
    log["gates"]["fullCTest"] = "PASS"

    if SAN_JOB in needed_jobs:
        san = review.get("san")
        if not isinstance(san, dict):
            raise ValueError("Missing affected sanitizer evidence")
        affected = san.get("tests")
        if not isinstance(affected, list) or not affected or not set(affected) <= required:
            raise ValueError("Affected sanitizer tests require exact evidence in the current combined suite")
        config = cache_options(archive(san["cache"], "san-cache", logs, log))
        if any(config.get(option) != "ON" for option in ("ENABLE_ASAN", "ENABLE_UBSAN", "XR_XIR_PARANOID")):
            raise ValueError("Affected sanitizer evidence lacks ASan, UBSan or paranoid mode")
        verify_current(san["current"], "ubuntu-24.04", candidate, required, logs, log, "Debug")
        log["gates"]["affectedSan"] = "PASS"
    else:
        log["gates"]["affectedSan"] = "NOT_APPLICABLE"


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
    verify_task339(integration, candidate, changed, review, logs, log)
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
               "state": "RUNNING", "gates": {"L1": "NOT_RUN", "fullCTest": "NOT_RUN", "affectedSan": "NOT_RUN"}}
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
