"""Explicit current model inputs without rewriting archived identities.

The old digest remains a historical input identity. A separate Task342 record
binds the preserved source path to its current nominal/Map program. This is
preparation metadata; only real product gates qualify execution.
"""
from pathlib import Path
import hashlib
import json
from frozen_derivation_inputs import resolve as resolve_derivation


def current_input(root: Path, relative: str, original_digest: str) -> Path:
    manifest = json.loads((root / "tests/aot/task342_structural_responsibilities.json").read_text(encoding="utf-8"))
    selected = [row for row in manifest["model_inputs"] if row["original_source"] == relative]
    if not selected:
        return resolve_derivation(root, relative, original_digest)
    if len(selected) != 1:
        raise ValueError("Task342 current input has no unique historical identity: " + relative)
    row = selected[0]
    if row["preserved_model_input_lf_sha256"] != original_digest or row["current_source"] != relative:
        raise ValueError("Task342 historical model input identity changed: " + relative)
    path = root / relative
    digest = hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    if digest != row["current_source_lf_sha256"]:
        raise ValueError("Task342 current nominal/Map input changed: " + relative)
    return path


def verify_input(root: Path, relative: str, original_digest: str) -> None:
    path = current_input(root, relative, original_digest)
    manifest = json.loads((root / "tests/aot/task342_structural_responsibilities.json").read_text(encoding="utf-8"))
    if any(row["original_source"] == relative for row in
           manifest["model_inputs"]):
        return
    if hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest() != original_digest:
        raise ValueError("Historical independent input changed: " + relative)
