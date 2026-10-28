#!/usr/bin/env python3
"""Integrate one reviewed local commit through a detached worktree and L1."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

import xray_l1
from xray_slot import Lease, save_json
from xray_process import RecoveryRequired


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


def clean(root: Path) -> int:
    """Refresh only blobs proven identical to HEAD, without changing file bytes."""
    if git(root, "diff", "--cached", "--name-only") or git(root, "ls-files", "--others", "--exclude-standard"):
        raise RuntimeError("Integration worktree has staged or untracked changes")
    paths = git(root, "diff-files", "--name-only").splitlines()
    for path in paths:
        if git(root, "hash-object", "--path=" + path, "--", path) != git(root, "rev-parse", "HEAD:" + path):
            raise RuntimeError(f"Integration worktree has changed content: {path}")
    if paths:
        git(root, "add", "--", *paths)
    if git(root, "status", "--porcelain"):
        raise RuntimeError("Integration worktree is not clean")
    return len(paths)


def review_request(args) -> dict:
    review = json.loads(args.review.read_text(encoding="utf-8-sig"))
    if review.get("approved") is not True:
        raise ValueError("Review has not approved this slice")
    for key in ("sourceSHA", "reviewedBase"):
        if not re.fullmatch(r"[0-9a-f]{40}", review.get(key, "")):
            raise ValueError(f"Review requires an exact {key}")
    roles = {"lead", "a", "b", "c", "d"}
    if review.get("authorRole") not in roles or review.get("reviewerRole") not in roles:
        raise ValueError("Review requires the author and reviewer roles")
    if review["authorRole"] == review["reviewerRole"]:
        raise ValueError("An independent reviewer is required")
    for key in ("reviewEvidence", "l0Log", "sliceAsanLog"):
        path = Path(review.get(key, ""))
        if not path.is_absolute() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Review requires an existing, nonempty {key}")
    return review


def slice_asan_gate(review: dict, changed: list[str], log: dict, logs: Path) -> None:
    """Archive the author's gate and enforce the independent review's verdict."""
    content = Path(review["sliceAsanLog"]).read_bytes()
    (logs / "slice-asan.log").write_bytes(content)
    result = review.get("sliceAsanResult")
    log["sliceAsanEvidence"] = {"path": review["sliceAsanLog"],
                                "sha256": hashlib.sha256(content).hexdigest(),
                                "result": result}
    log["gates"]["slice_asan"] = "FAIL"
    if result == "PASS":
        log["gates"]["slice_asan"] = "PASS"
        return
    native = any(p.startswith(("src/", "include/")) or
                 Path(p).suffix.lower() in {".c", ".h", ".cc", ".cpp", ".hpp"} for p in changed)
    reason = review.get("sliceAsanReason")
    if result == "NOT_APPLICABLE" and not native and isinstance(reason, str) and reason.strip():
        log["gates"]["slice_asan"] = "NOT_APPLICABLE: " + reason.strip()
        return
    raise RuntimeError("Slice ASan gate must pass; non-native slices require a reviewed reason")


def integrate(args, review: dict, log: dict, logs: Path) -> None:
    root, integration = args.repo.resolve(), args.integration.resolve()
    main = git(root, "rev-parse", "refs/heads/main")
    source = git(root, "rev-parse", "--verify", args.branch + "^{commit}")
    log.update(oldMain=main, sourceSHA=source)
    if source != review["sourceSHA"]:
        raise RuntimeError("Branch moved after the recorded review")
    base = review["reviewedBase"]
    if not ancestor(root, base, main) or not ancestor(root, base, source):
        raise RuntimeError("Reviewed base must be an ancestor of main and the reviewed commit")
    commits = git(root, "rev-list", "--reverse", base + ".." + source).splitlines()
    if not commits or git(root, "rev-list", "--merges", base + ".." + source):
        raise RuntimeError("The initial queue requires a nonempty, linear slice")
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
    changed = git(integration, "diff", "--name-only", main, candidate).splitlines()
    log["changed"] = changed
    slice_asan_gate(review, changed, log, logs)
    relative = Path(args.manifest)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("L1 manifest must be a tracked path inside the candidate")
    plan_blob = git(integration, "rev-parse", candidate + ":" + relative.as_posix())
    content = subprocess.check_output(["git", "-C", str(integration), "cat-file", "blob", plan_blob])
    plan = xray_l1.manifest(content)
    log["manifestBlob"] = plan_blob
    log["manifestSHA256"] = hashlib.sha256(content).hexdigest()
    log["smoke"] = plan.get("smokeStatus", "NOT_RUN: no smoke status supplied")
    environment = xray_l1.msvc_environment(args.temporary.resolve())
    with Lease(args.slots, "build", priority=0, payload={"candidate": candidate}):
        xray_l1.configure(integration, args.build, logs, environment)
        clean(integration)
        xray_l1.build_targets(args.build, plan["targets"], logs, environment)
        if clean(integration):
            xray_l1.build_targets(args.build, plan["targets"], logs, environment)
            clean(integration)
        log["identity"] = xray_l1.identity(plan, integration, args.build, candidate, logs, environment)
        groups = {"l1": plan["tests"]}
        if any(re.match(r"src/(ir|aot|analysis|lir|hir|xir|plan|program|module|vm/register)/", p) for p in changed):
            log["gates"]["meta_ownership_inventory"] = "FAIL"
            groups["metadata"] = ["meta_ownership_inventory"]
        results = xray_l1.test_groups(groups, args.build, logs, environment)
        log["gates"]["L1"] = "PASS" if results["l1"] else "FAIL"
        if "metadata" in results:
            log["gates"]["meta_ownership_inventory"] = "PASS" if results["metadata"] else "FAIL"
        if not all(results.values()):
            raise RuntimeError("L1 or metadata: failed, skipped or unexecuted test")
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
    parser.add_argument("--build", type=Path, default=Path(r"D:\xray-build\int-rel"))
    parser.add_argument("--temporary", type=Path, default=Path(r"D:\xray-build\int-tmp"))
    parser.add_argument("--queue", type=Path, default=Path(r"D:\xray-build\queue"))
    parser.add_argument("--slots", type=Path, default=Path(r"D:\xray-build\slots"))
    parser.add_argument("--manifest", default="scripts/l1-v0.json")
    args = parser.parse_args()
    for name in ("repo", "integration", "build", "temporary", "queue", "slots", "review"):
        setattr(args, name, getattr(args, name).resolve())
    review = review_request(args)
    started = time.time()
    with Lease(args.queue, "queue", capacity=1, payload={"branch": args.branch, "review": review}) as lease:
        logs = args.queue.resolve() / "logs" / f"{lease.record['sequence']:020d}"
        logs.mkdir(parents=True)
        log = {"schema": 1, "sequence": lease.record["sequence"], "branch": args.branch,
               "review": review, "started": started, "processingStarted": time.time(),
               "state": "RUNNING", "gates": {"L1": "FAIL", "slice_asan": "FAIL",
                    "asan_focused": "NOT_RUN: redlist-2026-10-21; restore when the full lane can build"}}
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
