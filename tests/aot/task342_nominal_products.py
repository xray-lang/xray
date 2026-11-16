"""Retain legal nominal/Map duties after retiring structural machine plans.

The inventory proves retirement/provenance only. The product group runs real
Source checks, VM execution, verified C generation and native execution. Six
unaccepted dependencies remain visible and are never converted into passes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/lib"))
from xraytest import proc

MANIFEST = Path(__file__).with_name("task342_structural_responsibilities.json")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def inventory() -> dict:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert manifest["schema"] == 1 and manifest["task"] == 342
    rows = manifest["retired_machine_oracles"]
    assert len(rows) == 19 and len({row["source"] for row in rows}) == 19
    assert len({row["retired_oracle"] for row in rows}) == 19
    assert sum(row["product_gate"] == "REQUIRED" for row in rows) == 13
    for row in rows:
        source = ROOT / row["source"]
        retired = ROOT / row["retired_oracle"]
        assert source.resolve().is_relative_to(ROOT / "tests/aot/filetests")
        assert retired.resolve().is_relative_to(ROOT / "tests/aot/filetests")
        assert not retired.exists(), f"deleted structural machine oracle returned: {retired}"
        assert digest(source) == row["current_source_lf_sha256"], source
        assert row["retained_responsibilities"] and row["retired_responsibilities"]
        assert row["model_is_product_evidence"] is False
        assert row["product_gate"] in ("REQUIRED", "UNACCEPTED_DEPENDENCY")
        assert bool(row["remaining_dependency"]) == (row["product_gate"] == "UNACCEPTED_DEPENDENCY")
    inputs = manifest["model_inputs"]
    assert len(inputs) == 6 and len({row["original_source"] for row in inputs}) == 6
    for row in inputs:
        assert row["original_source"] == row["current_source"]
        assert digest(ROOT / row["current_source"]) == row["current_source_lf_sha256"]
        assert len(row["preserved_model_input_lf_sha256"]) == 64
    observations=manifest.get("preexisting_input_observations",[])
    assert len(observations)==7 and len({row["original_source"] for row in observations})==7
    for row in observations:
        assert row["task342_modified"] is False and row["base_8c050f296_lf_sha256"]==row["current_source_lf_sha256"]
    authorities=manifest.get("preexisting_authority_observations",[])
    assert len(authorities)==4 and len({row["original_source"] for row in authorities})==4
    for row in authorities:
        assert row["original_source"]==row["current_source"] and row["task342_modified"] is False
        assert row["base_8c050f296_lf_sha256"]==row["current_source_lf_sha256"]
        assert row["scope"].startswith("PREEXISTING_DIVERGENCE_ONLY")
    return manifest


def command(arguments: list[str], cwd: Path, expected: bytes | None = None) -> None:
    result = proc.run(arguments, cwd=cwd, timeout=30, env={**os.environ, "NO_COLOR": "1"})
    if not result.ok:
        raise AssertionError((arguments, result.returncode, result.timed_out,
                              result.stdout.decode("utf-8", "replace"),
                              result.stderr.decode("utf-8", "replace")))
    if expected is not None:
        assert result.stdout.replace(b"\r\n", b"\n") == expected, (arguments, result.stdout, expected)
        assert not result.stderr, (arguments, result.stderr)


def products(binary: Path, manifest: dict, include_unaccepted: bool) -> None:
    selected = [row for row in manifest["retired_machine_oracles"]
                if include_unaccepted or row["product_gate"] == "REQUIRED"]
    with tempfile.TemporaryDirectory(prefix="xray-task342-nominal-") as temporary:
        folder = Path(temporary)
        for index, row in enumerate(selected):
            source = ROOT / row["source"]
            expected = row["expected_stdout_utf8"].encode("utf-8")
            command([str(binary), "check", str(source)], ROOT)
            command([str(binary), "run", str(source)], ROOT, expected)
            generated = folder / f"case-{index}.c"
            command([str(binary), "build", "-c", "-o", str(generated), str(source)], ROOT)
            assert generated.is_file() and generated.stat().st_size > 0
            native = folder / (f"case-{index}.exe" if os.name == "nt" else f"case-{index}")
            command([str(binary), "build", "-o", str(native), str(source)], ROOT)
            command([str(native)], ROOT, expected)
            print(f"{source.name}: Source, VM, verified C and native values PASS", flush=True)
    remaining = 19 - len(selected)
    print(f"{len(selected)} real products passed; {remaining} retained dependencies remain unaccepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--inventory-only", action="store_true")
    parser.add_argument("--include-unaccepted", action="store_true",
                        help="Require every retained product, including currently unaccepted dependencies")
    args = parser.parse_args()
    manifest = inventory()
    if args.inventory_only:
        print("19 retired machine identities, 6 migrated model inputs and 7 historical observations validated; no product claim")
        return 0
    assert args.compiler, "--compiler is required for real products"
    products(args.compiler.resolve(strict=True), manifest, args.include_unaccepted)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
