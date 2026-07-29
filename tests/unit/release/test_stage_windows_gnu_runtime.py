from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

from stage_windows_gnu_runtime import stage


def write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def write_archive(path: Path, body: bytes) -> None:
    path.write_bytes(b"!<arch>\n" + body)


class StageWindowsGnuRuntimeTest(unittest.TestCase):
    def make_payload(self, root: Path, release_target: str = "windows-x86_64") -> None:
        write_json(
            root / "share/xray/install/install-marker.json",
            {
                "schema": 1,
                "product": "xray-lang",
                "layout": "xray-payload-v1",
                "target": release_target,
            },
        )
        write_json(
            root / "lib/xray/aot/x86_64-windows-msvc/manifest.json",
            {
                "schema": 1,
                "sdkAbi": 1,
                "target": "x86_64-windows-msvc",
                "objectFormat": "coff",
                "providers": ["msvc"],
                "artifacts": [],
                "systemLibraries": [],
            },
        )

    def test_stages_separate_zig_abi_with_exact_digests(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            root = work / "payload"
            self.make_payload(root)
            aot_core = work / "libxray_aot_core.a"
            rt_coro = work / "libxray_rt_coro.a"
            write_archive(aot_core, b"aot-core")
            write_archive(rt_coro, b"rt-coro")

            manifest_path = stage(root, "x86_64-windows-gnu", aot_core, rt_coro)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["target"], "x86_64-windows-gnu")
            self.assertEqual(manifest["providers"], ["zig"])
            self.assertEqual(manifest["objectFormat"], "coff")
            self.assertEqual(manifest["systemLibraries"], ["ws2_32"])
            self.assertEqual(
                [item["path"] for item in manifest["artifacts"]],
                ["libxray_aot_core.a", "libxray_rt_coro.a"],
            )
            for item in manifest["artifacts"]:
                archive = manifest_path.parent / item["path"]
                self.assertEqual(item["sha256"], hashlib.sha256(archive.read_bytes()).hexdigest())

    def test_rejects_msvc_archive_spelling(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            root = work / "payload"
            self.make_payload(root)
            aot_core = work / "xray_aot_core.lib"
            rt_coro = work / "libxray_rt_coro.a"
            write_archive(aot_core, b"msvc")
            write_archive(rt_coro, b"gnu")
            with self.assertRaisesRegex(ValueError, "libxray_aot_core.a"):
                stage(root, "x86_64-windows-gnu", aot_core, rt_coro)

    def test_rejects_payload_architecture_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            root = work / "payload"
            self.make_payload(root, "windows-arm64")
            aot_core = work / "libxray_aot_core.a"
            rt_coro = work / "libxray_rt_coro.a"
            write_archive(aot_core, b"aot")
            write_archive(rt_coro, b"coro")
            with self.assertRaisesRegex(ValueError, "expected target windows-x86_64"):
                stage(root, "x86_64-windows-gnu", aot_core, rt_coro)


if __name__ == "__main__":
    unittest.main()
