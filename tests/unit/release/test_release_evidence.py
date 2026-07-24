from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[3]
SCRIPT = ROOT / "scripts" / "generate_release_evidence.py"


class ReleaseEvidenceContractTest(unittest.TestCase):
    def test_release_metadata_binds_archive_and_payload_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            payload = work / "payload"
            manifest_path = payload / "share" / "xray" / "install" / "payload-manifest.json"
            manifest_path.parent.mkdir(parents=True)
            manifest = {
                "schema": 1,
                "product": "xray-lang",
                "version": "1.2.3",
                "commit": "a" * 40,
                "dirty": False,
                "target": "linux-x86_64",
                "buildProfile": "Release",
                "files": [
                    {
                        "path": "bin/xray",
                        "size": 4,
                        "sha256": hashlib.sha256(b"xray").hexdigest(),
                    }
                ],
            }
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            artifact = work / "xray-linux-x64.tar.gz"
            artifact.write_bytes(b"immutable payload archive")
            output = work / "xray-linux-x64"
            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--root",
                    str(payload),
                    "--artifact",
                    str(artifact),
                    "--output-prefix",
                    str(output),
                    "--commit",
                    "a" * 40,
                    "--builder",
                    "test",
                    "--created",
                    "2026-07-24T00:00:00Z",
                ],
                check=True,
            )
            release = json.loads((work / "xray-linux-x64.release.json").read_text(encoding="utf-8"))
            self.assertEqual(release["schema"], 1)
            self.assertEqual(release["version"], "1.2.3")
            self.assertEqual(release["target"], "linux-x86_64")
            self.assertEqual(release["artifact"]["name"], artifact.name)
            self.assertEqual(release["artifact"]["size"], artifact.stat().st_size)
            self.assertEqual(release["artifact"]["sha256"], hashlib.sha256(artifact.read_bytes()).hexdigest())
            self.assertEqual(
                release["payloadManifest"]["sha256"], hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            )


if __name__ == "__main__":
    unittest.main()
