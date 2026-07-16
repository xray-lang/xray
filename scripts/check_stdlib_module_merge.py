#!/usr/bin/env python3
"""Task-196 S0 queue, module-branch intake, regeneration and merge gates."""

from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable

from stdlib_manifest import load_manifest, load_toml


INTAKE_PATH = Path("stdlib/stdlib_module_intake.toml")


def pattern_base(pattern: str) -> str:
    if pattern.endswith("/**"):
        return pattern[:-3].rstrip("/")
    return pattern


def pattern_is_ignored(root: Path, pattern: str) -> bool:
    base = pattern_base(pattern)
    result = subprocess.run(
        ["git", "check-ignore", "-q", "--", base],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def pattern_has_target(root: Path, pattern: str) -> bool:
    if pattern.endswith("/**"):
        return (root / pattern_base(pattern)).exists() or pattern_is_ignored(root, pattern)
    if any(char in pattern for char in "*?["):
        return any(root.glob(pattern)) or pattern_is_ignored(root, pattern)
    return (root / pattern).exists() or pattern_is_ignored(root, pattern)


def patterns_overlap(left: str, right: str) -> bool:
    if left == right:
        return True
    left_recursive = left.endswith("/**")
    right_recursive = right.endswith("/**")
    left_base = pattern_base(left)
    right_base = pattern_base(right)
    if left_recursive and right_recursive:
        return (
            left_base == right_base
            or left_base.startswith(right_base + "/")
            or right_base.startswith(left_base + "/")
        )
    if left_recursive:
        return path_matches(left, right_base)
    if right_recursive:
        return path_matches(right, left_base)
    return False


def load_intake(root: Path) -> dict[str, Any]:
    path = root / INTAKE_PATH
    if not path.is_file():
        raise RuntimeError(f"missing stdlib module intake manifest: {path}")
    return load_toml(path)


def validate_intake(data: dict[str, Any], root: Path | None = None) -> list[str]:
    errors: list[str] = []
    if data.get("schema") != 1:
        errors.append(f"{INTAKE_PATH}: schema must be 1")
    if not str(data.get("baseline_ref", "")).strip():
        errors.append(f"{INTAKE_PATH}: baseline_ref must be non-empty")
    s0 = data.get("s0", {})
    for field in ("owned_patterns", "generated_patterns", "contract_exempt_modules"):
        value = s0.get(field)
        if not isinstance(value, list):
            errors.append(f"{INTAKE_PATH}: s0.{field} must be a list")
    for field in ("owned_patterns", "generated_patterns"):
        patterns = [str(value) for value in s0.get(field, ())]
        seen: set[str] = set()
        for pattern in patterns:
            if pattern in seen:
                errors.append(f"{INTAKE_PATH}: duplicate s0.{field} pattern {pattern}")
            seen.add(pattern)
            if root is not None and not pattern_has_target(root, pattern):
                errors.append(f"{INTAKE_PATH}: s0.{field} pattern matches no path: {pattern}")
    owned_patterns = [str(value) for value in s0.get("owned_patterns", ())]
    generated_patterns = [str(value) for value in s0.get("generated_patterns", ())]
    for owned in owned_patterns:
        for generated in generated_patterns:
            if patterns_overlap(owned, generated):
                errors.append(
                    f"{INTAKE_PATH}: s0.owned_patterns pattern {owned} overlaps generated pattern {generated}"
                )
    lanes = data.get("lane", ())
    slots: set[str] = set()
    branches: set[str] = set()
    tasks: set[int] = set()
    for index, lane in enumerate(lanes, 1):
        missing = sorted({"slot", "task", "branch", "scope"} - set(lane))
        if missing:
            errors.append(f"{INTAKE_PATH}: lane {index} misses {', '.join(missing)}")
        slot = str(lane.get("slot", ""))
        branch = str(lane.get("branch", ""))
        task = lane.get("task")
        if slot in slots:
            errors.append(f"{INTAKE_PATH}: duplicate lane slot {slot}")
        if branch in branches:
            errors.append(f"{INTAKE_PATH}: duplicate lane branch {branch}")
        if isinstance(task, int) and task in tasks:
            errors.append(f"{INTAKE_PATH}: duplicate lane task {task}")
        slots.add(slot)
        branches.add(branch)
        if isinstance(task, int):
            tasks.add(task)
        else:
            errors.append(f"{INTAKE_PATH}: lane {slot or index} task must be an integer")
    if not lanes:
        errors.append(f"{INTAKE_PATH}: at least one lane is required")
    return errors


def _git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *args],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=check,
    )


def ref_commit(root: Path, ref: str) -> str | None:
    result = _git(root, "rev-parse", "--verify", f"{ref}^{{commit}}", check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def ahead_behind(root: Path, base: str, candidate: str) -> tuple[int, int]:
    result = _git(root, "rev-list", "--left-right", "--count", f"{base}...{candidate}")
    base_ahead, candidate_ahead = result.stdout.split()
    return int(base_ahead), int(candidate_ahead)


def changed_paths(root: Path, base: str, candidate: str) -> tuple[str, list[str]]:
    merge_base = _git(root, "merge-base", base, candidate).stdout.strip()
    result = _git(
        root,
        "diff",
        "--name-only",
        "--diff-filter=ACDMRTUXB",
        f"{merge_base}..{candidate}",
    )
    return merge_base, sorted(path for path in result.stdout.splitlines() if path)


def path_matches(pattern: str, path: str) -> bool:
    if pattern.endswith("/**"):
        prefix = pattern[:-3].rstrip("/")
        return path == prefix or path.startswith(prefix + "/")
    return fnmatch.fnmatchcase(path, pattern)


def owned_collisions(data: dict[str, Any], paths: Iterable[str]) -> list[dict[str, str]]:
    s0 = data.get("s0", {})
    patterns = [
        *(str(value) for value in s0.get("owned_patterns", ())),
        *(str(value) for value in s0.get("generated_patterns", ())),
    ]
    collisions: list[dict[str, str]] = []
    for path in paths:
        for pattern in patterns:
            if path_matches(pattern, path):
                collisions.append({"path": path, "pattern": pattern})
                break
    return collisions


def infer_modules(paths: Iterable[str]) -> list[str]:
    modules: set[str] = set()
    for raw in paths:
        parts = Path(raw).parts
        if len(parts) >= 3 and parts[0] == "stdlib" and parts[1] not in {"defs", "types"}:
            modules.add(parts[1])
        if len(parts) >= 4 and parts[:3] == ("tests", "stdlib", "contracts"):
            modules.add(parts[3])
        if len(parts) >= 5 and parts[:3] == ("tests", "benchmarks", "stdlib"):
            if parts[3] not in {"baselines", "results"}:
                modules.add(parts[3])
    return sorted(modules)


def candidate_has_path(root: Path, candidate: str, path: str) -> bool:
    result = _git(root, "cat-file", "-e", f"{candidate}:{path}", check=False)
    return result.returncode == 0


def audit_candidate(
    root: Path,
    data: dict[str, Any],
    base: str,
    candidate: str,
) -> dict[str, Any]:
    base_commit = ref_commit(root, base)
    candidate_commit = ref_commit(root, candidate)
    if base_commit is None:
        return {"status": "blocked", "violations": [f"base ref does not exist: {base}"]}
    if candidate_commit is None:
        return {"status": "missing", "violations": [f"candidate ref does not exist: {candidate}"]}
    base_ahead, candidate_ahead = ahead_behind(root, base, candidate)
    merge_base, paths = changed_paths(root, base, candidate)
    collisions = owned_collisions(data, paths)
    modules = infer_modules(paths)
    violations = [
        f"candidate owns S0 path {entry['path']} (pattern {entry['pattern']})"
        for entry in collisions
    ]
    exempt = set(data.get("s0", {}).get("contract_exempt_modules", ()))
    missing_contract_assets: list[str] = []
    for module in modules:
        if module in exempt:
            continue
        for leaf in ("contract.toml", "cases.jsonl"):
            rel = f"tests/stdlib/contracts/{module}/{leaf}"
            if not candidate_has_path(root, candidate, rel):
                missing_contract_assets.append(rel)
                violations.append(f"module {module} lacks required candidate asset {rel}")
    boundary = load_manifest(root)
    unregistered_modules = sorted(set(modules) - set(boundary.by_name))
    if candidate_ahead == 0:
        status = "waiting"
    elif violations:
        status = "blocked"
    elif base_ahead:
        status = "needs-sync"
    else:
        status = "ready"
    s0_actions: list[str] = []
    if modules:
        s0_actions.extend(
            (
                "reconcile stdlib_boundary.toml and dynamic audit from the integrated source",
                "reconcile correctness contracts and performance manifest entries",
                "regenerate stdlib analyzer/LSP/MCP/API artifacts",
                "refresh the source-derived self-hosting completion report",
            )
        )
    if unregistered_modules:
        s0_actions.append("add boundary rows for: " + ", ".join(unregistered_modules))
    return {
        "status": status,
        "base": base,
        "base_commit": base_commit,
        "candidate": candidate,
        "candidate_commit": candidate_commit,
        "merge_base": merge_base,
        "base_ahead": base_ahead,
        "candidate_ahead": candidate_ahead,
        "changed_paths": paths,
        "modules": modules,
        "unregistered_modules": unregistered_modules,
        "missing_contract_assets": missing_contract_assets,
        "collisions": collisions,
        "violations": violations,
        "s0_actions": s0_actions,
    }


def lane_by_slot(data: dict[str, Any], slot: str) -> dict[str, Any]:
    for lane in data.get("lane", ()):
        if lane.get("slot") == slot:
            return lane
    raise RuntimeError(f"unknown stdlib intake lane: {slot}")


def branch_worktrees(root: Path) -> dict[str, Path]:
    result = _git(root, "worktree", "list", "--porcelain")
    out: dict[str, Path] = {}
    worktree: Path | None = None
    for line in result.stdout.splitlines():
        if line.startswith("worktree "):
            worktree = Path(line.removeprefix("worktree "))
        elif line.startswith("branch refs/heads/") and worktree is not None:
            branch = line.removeprefix("branch refs/heads/")
            out[branch] = worktree
        elif not line:
            worktree = None
    return out


def worktree_dirty_paths(path: Path) -> list[str]:
    result = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=all"],
        cwd=path,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        return [f"<status unavailable: {result.stderr.strip()}>"]
    return [line for line in result.stdout.splitlines() if line]


def queue_status(audit_status: str, dirty_paths: list[str]) -> str:
    if audit_status in {"missing", "blocked"}:
        return audit_status
    if dirty_paths:
        return "in-progress"
    return audit_status


def queue_report(root: Path, data: dict[str, Any], base: str) -> dict[str, Any]:
    lanes: list[dict[str, Any]] = []
    worktrees = branch_worktrees(root)
    for lane in data.get("lane", ()):
        branch = str(lane["branch"])
        audit = audit_candidate(root, data, base, branch)
        worktree = worktrees.get(branch)
        dirty_paths = worktree_dirty_paths(worktree) if worktree else []
        audit["committed_status"] = audit["status"]
        audit["status"] = queue_status(str(audit["status"]), dirty_paths)
        audit["worktree"] = str(worktree) if worktree else None
        audit["dirty_paths"] = dirty_paths
        lanes.append({**lane, **audit})
    return {
        "schema": 1,
        "baseline_ref": data["baseline_ref"],
        "base": base,
        "base_commit": ref_commit(root, base),
        "lanes": lanes,
    }


def render_queue(report: dict[str, Any]) -> str:
    lines = [
        f"stdlib module intake queue: base={report['base']} "
        f"({report.get('base_commit') or 'missing'})"
    ]
    for lane in report["lanes"]:
        modules = ",".join(lane.get("modules", ())) or "-"
        dirty_count = len(lane.get("dirty_paths", ()))
        lines.append(
            f"{lane['slot']} task-{lane['task']} {lane['branch']}: {lane['status']} "
            f"(candidate_ahead={lane.get('candidate_ahead', 0)}, "
            f"base_ahead={lane.get('base_ahead', 0)}, dirty={dirty_count}, modules={modules})"
        )
        for violation in lane.get("violations", ()):
            lines.append(f"  BLOCK: {violation}")
    return "\n".join(lines)


def run_step(root: Path, label: str, command: list[str], stdout: Path | None = None) -> None:
    print(f"== {label} ==")
    if stdout is None:
        result = subprocess.run(command, cwd=root)
    else:
        stdout.parent.mkdir(parents=True, exist_ok=True)
        with stdout.open("w", encoding="utf-8") as handle:
            result = subprocess.run(command, cwd=root, stdout=handle)
    if result.returncode:
        raise RuntimeError(f"{label} failed with exit code {result.returncode}")


def run_source_checks(root: Path, modules: list[str], require_complete: bool) -> None:
    python = sys.executable
    steps = [
        (
            "stdlib declarative metadata sync",
            [python, "tools/stdlibgen/stdlibgen.py", "--root", str(root), "--check"],
        ),
        ("analyzer/LSP stdlib metadata sync", [python, "scripts/gen_stdlib_types.py", "--check"]),
        (
            "language and knowledge generated sync",
            [python, "scripts/gen_language_docs.py", "--root", str(root), "--check"],
        ),
        (
            "stdlib boundary governance",
            [python, "scripts/check_stdlib_boundary.py", "--root", str(root), "--check", "all"],
        ),
        (
            "stdlib performance manifest",
            [python, "tests/benchmarks/stdlib/run.py", "--validate-only"],
        ),
        (
            "stdlib .def migration residue",
            [python, "scripts/check_stdlib_def_migration_residue.py", "--root", str(root)],
        ),
        (
            "stdlib AOT helper residue",
            [python, "scripts/check_stdlib_aot_helper_residue.py", "--root", str(root)],
        ),
    ]
    for label, command in steps:
        run_step(root, label, command)
    if modules:
        for module in modules:
            run_step(
                root,
                f"stdlib migration contract: {module}",
                [python, "scripts/stdlib_migration.py", "check", module, "--root", str(root)],
            )
    else:
        run_step(
            root,
            "all stdlib migration contracts",
            [python, "scripts/stdlib_migration.py", "check", "--root", str(root)],
        )
    report_command = [python, "scripts/report_stdlib_self_hosting.py", "--root", str(root)]
    report_command.append("--require-complete" if require_complete else "--check")
    run_step(root, "stdlib self-hosting report", report_command)


def run_tooling_check(root: Path, xray: Path) -> None:
    python = sys.executable
    with tempfile.TemporaryDirectory(prefix="xray-stdlib-intake-") as tmp:
        temp = Path(tmp)
        builtin_dump = temp / "builtin_dump.json"
        inventory = temp / "api_inventory.json"
        run_step(root, "builtin API dump", [str(xray), "builtin-dump"], stdout=builtin_dump)
        run_step(
            root,
            "source API inventory and docs coverage",
            [
                python,
                "scripts/gen_api_inventory.py",
                "--root",
                str(root),
                "--builtin-dump",
                str(builtin_dump),
                "--json",
                str(inventory),
                "--check-docs",
            ],
        )
        run_step(
            root,
            "MCP knowledge generated sync",
            [
                python,
                "scripts/gen_mcp_knowledge.py",
                "--docs",
                str(root / "docs/knowledge"),
                "--spec",
                str(root / "LANGUAGE_SPEC_CN.md"),
                "--api-inventory",
                str(inventory),
                "--out",
                str(root / "src/app/mcp/xmcp_knowledge_generated.c"),
                "--check",
            ],
        )


def regenerate(root: Path, xray: Path, artifact_dir: Path) -> None:
    python = sys.executable
    run_step(root, "regenerate stdlib declarative metadata", [python, "tools/stdlibgen/stdlibgen.py", "--root", str(root)])
    run_step(root, "regenerate analyzer/LSP stdlib metadata", [python, "scripts/gen_stdlib_types.py"])
    run_step(root, "regenerate language and knowledge docs", [python, "scripts/gen_language_docs.py", "--root", str(root)])
    artifact_dir.mkdir(parents=True, exist_ok=True)
    builtin_dump = artifact_dir / "builtin_dump.json"
    inventory = artifact_dir / "api_inventory.json"
    inventory_html = artifact_dir / "api_inventory.html"
    run_step(root, "regenerate builtin API dump", [str(xray), "builtin-dump"], stdout=builtin_dump)
    run_step(
        root,
        "regenerate source API inventory",
        [
            python,
            "scripts/gen_api_inventory.py",
            "--root",
            str(root),
            "--builtin-dump",
            str(builtin_dump),
            "--json",
            str(inventory),
            "--html",
            str(inventory_html),
            "--check-docs",
        ],
    )
    run_step(
        root,
        "regenerate MCP knowledge",
        [
            python,
            "scripts/gen_mcp_knowledge.py",
            "--docs",
            str(root / "docs/knowledge"),
            "--spec",
            str(root / "LANGUAGE_SPEC_CN.md"),
            "--api-inventory",
            str(inventory),
            "--out",
            str(root / "src/app/mcp/xmcp_knowledge_generated.c"),
        ],
    )
    run_step(
        root,
        "regenerate stdlib self-hosting report",
        [
            python,
            "scripts/report_stdlib_self_hosting.py",
            "--root",
            str(root),
            "--json",
            str(artifact_dir / "stdlib_self_hosting.json"),
            "--markdown",
            str(artifact_dir / "stdlib_self_hosting.md"),
        ],
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    subparsers = parser.add_subparsers(dest="command", required=True)

    queue_parser = subparsers.add_parser("queue", help="show S1/S2/S3 branches waiting for S0")
    queue_parser.add_argument("--base", default="HEAD")
    queue_parser.add_argument("--json", action="store_true")
    queue_parser.add_argument("--strict", action="store_true")

    preflight_parser = subparsers.add_parser("preflight", help="audit one candidate branch")
    preflight_parser.add_argument("--base", default="HEAD")
    preflight_parser.add_argument("--candidate")
    preflight_parser.add_argument("--lane", choices=("S1", "S2", "S3"))
    preflight_parser.add_argument("--json", action="store_true")

    postmerge_parser = subparsers.add_parser("postmerge", help="check integrated S0 source and generated artifacts")
    postmerge_parser.add_argument("--module", action="append", default=[])
    postmerge_parser.add_argument("--xray", type=Path)
    postmerge_parser.add_argument("--require-complete", action="store_true")

    regen_parser = subparsers.add_parser("regenerate", help="regenerate all S0-owned stdlib artifacts")
    regen_parser.add_argument("--xray", required=True, type=Path)
    regen_parser.add_argument("--artifact-dir", default="build/stdlib-governance", type=Path)
    regen_parser.add_argument("--module", action="append", default=[])

    args = parser.parse_args()
    root = Path(args.root).resolve()
    try:
        data = load_intake(root)
        errors = validate_intake(data, root)
        if errors:
            raise RuntimeError("\n".join(errors))
        if args.command == "queue":
            report = queue_report(root, data, args.base)
            print(json.dumps(report, indent=2, sort_keys=True) if args.json else render_queue(report))
            if args.strict and any(lane["status"] in {"missing", "blocked"} for lane in report["lanes"]):
                return 1
            return 0
        if args.command == "preflight":
            candidate = args.candidate
            if args.lane:
                lane = lane_by_slot(data, args.lane)
                if candidate and candidate != lane["branch"]:
                    raise RuntimeError(
                        f"lane {args.lane} is registered as {lane['branch']}, not {candidate}"
                    )
                candidate = str(lane["branch"])
            if not candidate:
                raise RuntimeError("preflight requires --candidate or --lane")
            audit = audit_candidate(root, data, args.base, candidate)
            print(json.dumps(audit, indent=2, sort_keys=True) if args.json else render_queue({
                "base": args.base,
                "base_commit": audit.get("base_commit"),
                "lanes": [{"slot": args.lane or "candidate", "task": "?", "branch": candidate, **audit}],
            }))
            return 0 if audit["status"] in {"ready", "needs-sync"} else 1
        if args.command == "postmerge":
            run_source_checks(root, args.module, args.require_complete)
            if args.xray:
                run_tooling_check(root, args.xray.resolve())
            print("OK: task-196 stdlib module post-merge gate passed")
            return 0
        if args.command == "regenerate":
            artifact_dir = args.artifact_dir
            if not artifact_dir.is_absolute():
                artifact_dir = root / artifact_dir
            regenerate(root, args.xray.resolve(), artifact_dir.resolve())
            run_source_checks(root, args.module, False)
            run_tooling_check(root, args.xray.resolve())
            print(f"OK: task-196 S0 artifacts regenerated under {artifact_dir}")
            return 0
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"stdlib module merge gate failed: {exc}", file=sys.stderr)
        return 1
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
