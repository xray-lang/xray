#!/usr/bin/env python3
"""Drive every target-machine evidence collector and publish the assembled set."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))
import assemble_target_machine_completion_evidence as assembler  # noqa: E402
import check_target_machine_completion as completion  # noqa: E402


# Every governed lane, in the order that surfaces the cheapest rejection first.
LANE_COLLECTORS = (
    ("symbol", "collect_target_machine_symbol_evidence.py"),
    ("dependency-graph", "collect_target_machine_dependency_graph_evidence.py"),
    ("installed", "collect_target_machine_installed_evidence.py"),
    ("runtime-reachability", "collect_target_machine_runtime_reachability_evidence.py"),
    ("activation-generation", "collect_target_machine_activation_generation_evidence.py"),
    ("matrix", "collect_target_machine_matrix_evidence.py"),
    ("full-validation", "collect_target_machine_full_validation_evidence.py"),
)


class DriverError(ValueError):
    pass


def lane_command(root: Path, build: Path, script: str, kind: str, package: Path,
                 owner: str, row_results: Path, lane_timeout: int) -> list[str]:
    command = [sys.executable, str(root / "scripts" / script), "--root", str(root)]
    if kind == "matrix":
        command += ["--row-results-dir", str(row_results)]
    else:
        command += ["--build", str(build)]
    command += ["--output-dir", str(package), "--owner", owner]
    if kind == "full-validation":
        command += ["--lane-timeout", str(lane_timeout)]
    return command


def run_lane(command: list[str], root: Path) -> int:
    print(f"  $ {' '.join(command)}", flush=True)
    return subprocess.run(command, cwd=root, check=False).returncode


def merge_package(kind: str, package: Path, assembly: Path) -> str:
    """Copy one collector package into the flat root the assembler reads."""
    manifest = package / f"{kind}.raw.json"
    if not manifest.is_file():
        raise DriverError(f"{kind} package has no raw manifest: {manifest}")
    destination = assembly / manifest.name
    if destination.exists():
        raise DriverError(f"{kind} raw manifest collides in the assembly root")
    shutil.copyfile(manifest, destination)
    for path in sorted(package.rglob("*")):
        if not path.is_file() or path == manifest:
            continue
        relative = path.relative_to(package)
        target = assembly / relative
        if target.exists():
            raise DriverError(
                f"{kind} retained evidence collides in the assembly root: {relative}"
            )
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    return manifest.name


def write_bundle(root: Path, assembly: Path, lanes: dict[str, str],
                 governance: dict[str, Any]) -> Path:
    identity, findings = completion.repository_identity(root)
    if findings:
        detail = "; ".join(row.message for row in findings)
        raise DriverError(f"source identity is not publishable: {detail}")
    bundle = {
        "schema": assembler.SCHEMA,
        "assembler": assembler.ASSEMBLER,
        "source_commit": identity["source_commit"],
        "repository_sha256": identity["repository_sha256"],
        "governance_input_sha256": completion.governance_input_sha256(root, governance),
        "lanes": lanes,
    }
    path = assembly / "bundle.json"
    assembler.write_object(path, bundle)
    return path


def collect(root: Path, build: Path, raw_root: Path, output: Path, owner: str,
            row_results: Path, lane_timeout: int) -> int:
    governance = assembler.read_object(root / completion.DEFAULT_MANIFEST)
    if completion.validate_manifest(governance):
        raise DriverError("completion governance manifest is invalid")
    required = set(governance["evidence"]["required_files"])
    if {kind for kind, _ in LANE_COLLECTORS} != required:
        raise DriverError("driver lane catalog is not the governed evidence catalog")
    if raw_root.exists() or raw_root.is_symlink():
        raise DriverError(
            f"raw evidence root already exists; collection never overwrites: {raw_root}"
        )
    packages = raw_root / "packages"
    assembly = raw_root / "assembly"
    packages.mkdir(parents=True)
    assembly.mkdir(parents=True)

    results: dict[str, int] = {}
    for kind, script in LANE_COLLECTORS:
        print(f"[{kind}] collecting", flush=True)
        results[kind] = run_lane(
            lane_command(root, build, script, kind, packages / kind, owner,
                         row_results, lane_timeout),
            root,
        )

    print("\ntarget-machine completion evidence lanes:")
    for kind, _ in LANE_COLLECTORS:
        verdict = "passed" if results[kind] == 0 else f"failed (exit {results[kind]})"
        print(f"  {kind}: {verdict}")
    failed = [kind for kind, code in results.items() if code != 0]
    if failed:
        print(
            "\nevidence was not published: "
            f"{len(failed)} of {len(results)} lanes did not pass: {', '.join(sorted(failed))}",
            file=sys.stderr,
        )
        return 1

    lanes = {
        kind: merge_package(kind, packages / kind, assembly)
        for kind, _ in LANE_COLLECTORS
    }
    bundle = write_bundle(root, assembly, lanes, governance)
    assembler.assemble(root, (root / completion.DEFAULT_MANIFEST).resolve(), bundle,
                       output)
    print(f"\npublished target-machine completion evidence: {output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".")
    parser.add_argument("--build", default="build")
    parser.add_argument("--owner", required=True)
    parser.add_argument("--raw-root")
    parser.add_argument("--output")
    parser.add_argument("--row-results-dir")
    parser.add_argument("--lane-timeout", type=int, default=14400)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    build = Path(args.build)
    if not build.is_absolute():
        build = root / build
    build = build.resolve()
    raw_root = Path(args.raw_root) if args.raw_root else build / "target-machine-raw-evidence"
    output = Path(args.output) if args.output else build / "target-machine-completion-evidence"
    row_results = (Path(args.row_results_dir) if args.row_results_dir
                   else build / "target-machine-matrix-row-results")
    try:
        return collect(root, build, raw_root.resolve(), output.resolve(), args.owner,
                       row_results.resolve(), args.lane_timeout)
    except (DriverError, assembler.AssemblyError, OSError, KeyError, TypeError,
            ValueError) as error:
        print(f"target-machine completion evidence: ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
