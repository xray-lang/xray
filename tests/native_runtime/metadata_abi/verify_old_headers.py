"""Verify the independent old producer's exact pre-cutover header bytes."""
from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parent / "old_headers"
manifest = json.loads((root / "provenance.json").read_text(encoding="utf-8"))
assert manifest["source_commit"] == "8c050f296b83bf551d0ef97f4669cf230621c564"
assert len(manifest["headers"]) == 3
for row in manifest["headers"]:
    path = root / Path(row["path"]).name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"], path
print("three exact pre-cutover public headers verified")
