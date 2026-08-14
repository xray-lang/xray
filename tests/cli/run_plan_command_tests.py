#!/usr/bin/env python3
"""Prove `xray plan` renders deterministically, verifies exactly, and locates
the first difference between two target plans."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=60,
        check=False,
    )


def require(condition: bool, message: str, output: str = "") -> None:
    if condition:
        print(f"  PASS: {message}")
        return
    raise AssertionError(f"{message}\n{output}")


def write_pair(writer: Path, root: Path, name: str, value: int,
               stem: str) -> tuple[Path, Path]:
    xsm = root / f"{stem}.xsm"
    xtp = root / f"{stem}.xtp"
    result = run([str(writer), name, str(value), str(xsm), str(xtp)])
    require(result.returncode == 0 and xsm.is_file() and xtp.is_file(),
            f"fixture writer produced the {stem} artifact pair",
            result.stdout + result.stderr)
    return xsm, xtp


def check_help(binary: Path) -> None:
    result = run([str(binary), "plan", "--help"])
    require(result.returncode == 0, "plan --help succeeds", result.stderr)
    for subcommand in ("dump", "verify", "diff"):
        require(subcommand in result.stdout,
                f"plan --help lists the {subcommand} subcommand", result.stdout)
    listing = run([str(binary), "--help"])
    require("plan" in listing.stdout, "top-level help lists plan", listing.stdout)


def check_dump(binary: Path, root: Path, xtp: Path) -> None:
    first = run([str(binary), "plan", "dump", str(xtp)])
    require(first.returncode == 0, "plan dump succeeds", first.stderr)
    for marker in ("xray-plan-dump", "identity.plan-fingerprint=",
                   "identity.completed-families=", "section FUNCTIONS",
                   "section SLOTS", "section CALLS", "section TARGET_PROFILE",
                   "[TARGET_PROFILE] 0", "[FUNCTIONS] 0", "[INSTRUCTIONS] 0",
                   "opcode-name="):
        require(marker in first.stdout, f"plan dump reports {marker!r}", first.stdout)

    second = run([str(binary), "plan", "dump", str(xtp)])
    require(second.stdout == first.stdout,
            "plan dump of one artifact is byte-identical across runs")

    # The rendering must depend on the artifact bytes alone: a byte-identical
    # copy at a different path renders identically.
    copied = root / "renamed-copy.xtp"
    shutil.copyfile(xtp, copied)
    third = run([str(binary), "plan", "dump", str(copied)])
    require(third.stdout == first.stdout,
            "plan dump depends on artifact bytes, not on the file path")
    require(str(xtp) not in first.stdout and str(root) not in first.stdout,
            "plan dump leaks neither the artifact path nor its directory",
            first.stdout)


def check_verify(binary: Path, xsm: Path, xtp: Path) -> None:
    unbound = run([str(binary), "plan", "verify", str(xtp)])
    require(unbound.returncode != 0,
            "plan verify refuses an artifact without a semantic authority")
    require("XR_ARTIFACT_2007" in unbound.stderr,
            "plan verify reuses the artifact authority diagnostic", unbound.stderr)

    bound = run([str(binary), "plan", "verify", str(xtp), "--semantic-plan", str(xsm)])
    require(bound.returncode == 0, "plan verify accepts a matched pair",
            bound.stdout + bound.stderr)
    for marker in ("plan-verify verified", "plan-fingerprint=", "functions=",
                   "family-mask="):
        require(marker in bound.stdout, f"plan verify reports {marker!r}", bound.stdout)


def check_verify_rejects_mismatch(binary: Path, xsm: Path, xtp: Path) -> None:
    mismatched = run([str(binary), "plan", "verify", str(xtp), "--semantic-plan", str(xsm)])
    require(mismatched.returncode != 0,
            "plan verify refuses a plan bound to a foreign semantic authority")
    require("plan-verify verified" not in mismatched.stdout,
            "plan verify never reports a pass it did not prove", mismatched.stdout)


def check_diff(binary: Path, base: Path, variant: Path, renamed: Path) -> None:
    same = run([str(binary), "plan", "diff", str(base), str(base)])
    require(same.returncode == 0, "plan diff exits 0 on identical artifacts", same.stderr)
    require("plan-diff identical" in same.stdout,
            "plan diff reports identical artifacts", same.stdout)

    changed = run([str(binary), "plan", "diff", str(base), str(variant)])
    require(changed.returncode != 0, "plan diff exits non-zero on a difference")
    require("first-difference" in changed.stdout,
            "plan diff reports a first difference", changed.stdout)
    require("section=INSTRUCTIONS" in changed.stdout,
            "a changed instruction immediate is located in the instruction table",
            changed.stdout)
    require("immediate_bits=" in changed.stdout,
            "plan diff renders the differing row's fields", changed.stdout)
    require(any(line.startswith("- ") for line in changed.stdout.splitlines()) and
            any(line.startswith("+ ") for line in changed.stdout.splitlines()),
            "plan diff shows both sides of the difference", changed.stdout)
    require("wire=" in changed.stdout,
            "plan diff shows the exact wire bytes of the differing row", changed.stdout)

    context = run([str(binary), "plan", "diff", str(base), str(variant), "--context", "0"])
    require(context.returncode != 0 and "first-difference" in context.stdout,
            "plan diff honours a zero context request", context.stdout)
    require(len(context.stdout.splitlines()) < len(changed.stdout.splitlines()),
            "zero context prints fewer lines than the default", context.stdout)

    # A renamed function keeps the same instruction immediates, so the report
    # must come from somewhere other than the instruction table.
    renamed_diff = run([str(binary), "plan", "diff", str(base), str(renamed)])
    require(renamed_diff.returncode != 0, "plan diff detects a renamed function")
    require("first-difference" in renamed_diff.stdout,
            "a renamed function still yields a located difference", renamed_diff.stdout)
    require("section=INSTRUCTIONS" not in renamed_diff.stdout,
            "a renamed function is not blamed on an instruction row", renamed_diff.stdout)


def check_rejections(binary: Path, root: Path, xsm: Path, xtp: Path) -> None:
    missing = run([str(binary), "plan", "dump", str(root / "absent.xtp")])
    require(missing.returncode != 0, "plan dump refuses a missing artifact")
    require("XR_ARTIFACT_" in missing.stderr,
            "a missing artifact keeps an artifact diagnostic code", missing.stderr)

    wrong_kind = run([str(binary), "plan", "dump", str(xsm)])
    require(wrong_kind.returncode != 0, "plan dump refuses a semantic module artifact")
    require("XR_ARTIFACT_" in wrong_kind.stderr,
            "a semantic module keeps an artifact diagnostic code", wrong_kind.stderr)

    truncated = root / "truncated.xtp"
    truncated.write_bytes(xtp.read_bytes()[:64])
    broken = run([str(binary), "plan", "dump", str(truncated)])
    require(broken.returncode != 0, "plan dump refuses a truncated artifact")
    require(broken.stderr.strip() != "",
            "a truncated artifact reports a decoder diagnostic", broken.stdout)

    bad_option = run([str(binary), "plan", "dump", str(xtp), "--semantic-plan", str(xsm)])
    require(bad_option.returncode != 0,
            "plan dump refuses an option that only applies to verify")

    unknown = run([str(binary), "plan", "explode", str(xtp)])
    require(unknown.returncode != 0, "plan refuses an unknown subcommand")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--fixture-writer", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    writer = args.fixture_writer.resolve(strict=True)

    with tempfile.TemporaryDirectory(prefix="xray-plan-command-") as temporary:
        root = Path(temporary)
        base_xsm, base_xtp = write_pair(writer, root, "plan_cli_probe", 42, "base")
        _, variant_xtp = write_pair(writer, root, "plan_cli_probe", 43, "variant")
        renamed_xsm, renamed_xtp = write_pair(writer, root, "plan_cli_other", 42, "renamed")

        check_help(binary)
        check_dump(binary, root, base_xtp)
        check_verify(binary, base_xsm, base_xtp)
        check_verify_rejects_mismatch(binary, renamed_xsm, base_xtp)
        check_diff(binary, base_xtp, variant_xtp, renamed_xtp)
        check_rejections(binary, root, base_xsm, base_xtp)

    print("plan command tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
