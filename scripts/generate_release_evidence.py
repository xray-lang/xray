#!/usr/bin/env python3
"""Generate deterministic SPDX inventory, artifact digest, and provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--output-prefix", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--builder", required=True)
    parser.add_argument("--created", required=True, help="RFC 3339 source commit timestamp")
    args = parser.parse_args()

    manifest_path = args.root / "share/xray/install/payload-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    files = []
    for entry in sorted(manifest["files"], key=lambda item: item["path"]):
        if entry.get("kind") == "symlink":
            continue
        files.append({
            "SPDXID": "SPDXRef-File-" + hashlib.sha256(entry["path"].encode()).hexdigest()[:16],
            "fileName": "./" + entry["path"],
            "checksums": [{"algorithm": "SHA256", "checksumValue": entry["sha256"]}],
        })
    namespace = f"https://xray-lang.org/spdx/xray-{manifest['version']}-{manifest['commit']}-{manifest['target']}"
    sbom = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"xray-{manifest['version']}-{manifest['target']}",
        "documentNamespace": namespace,
        "creationInfo": {
            "created": args.created,
            "creators": ["Organization: Xray Language", "Tool: generate_release_evidence.py"],
        },
        "packages": [{
            "name": "xray-lang",
            "SPDXID": "SPDXRef-Package-Xray",
            "versionInfo": manifest["version"],
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": True,
            "licenseConcluded": "MIT",
            "licenseDeclared": "MIT",
            "copyrightText": "NOASSERTION",
        }],
        "files": files,
        "relationships": [
            {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Package-Xray"},
            *[{"spdxElementId": "SPDXRef-Package-Xray", "relationshipType": "CONTAINS", "relatedSpdxElement": item["SPDXID"]} for item in files],
        ],
    }
    artifact_sha = digest(args.artifact)
    provenance = {
        "_type": "https://in-toto.io/Statement/v1",
        "subject": [{"name": args.artifact.name, "digest": {"sha256": artifact_sha}}],
        "predicateType": "https://slsa.dev/provenance/v1",
        "predicate": {
            "buildDefinition": {"buildType": "https://xray-lang.org/build/release-v1", "externalParameters": {"commit": args.commit}},
            "runDetails": {"builder": {"id": args.builder}},
        },
    }
    args.output_prefix.with_suffix(".spdx.json").write_text(json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    args.output_prefix.with_suffix(".provenance.json").write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    args.output_prefix.with_suffix(".sha256").write_text(f"{artifact_sha}  {args.artifact.name}\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
