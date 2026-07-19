#!/usr/bin/env python3
"""ParamMode coverage for the source-derived API inventory."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import gen_api_inventory as api_inventory  # noqa: E402


class ApiInventoryParamModeTest(unittest.TestCase):
    def test_normalized_signatures_keep_param_modes(self) -> None:
        self.assertEqual(
            "(a: in int, b: ref string, c: out bool): ()",
            api_inventory.normalize_signature(
                " a: in int,\n    b: ref string,\n    c: out bool ", None
            ),
        )

    def test_pure_stdlib_inventory_keeps_param_modes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-api-inventory-param-modes.") as tmp:
            root = Path(tmp)
            module_dir = root / "stdlib" / "modes"
            module_dir.mkdir(parents=True)
            (module_dir / "modes.xr").write_text(
                "\n".join(
                    [
                        "export fn borrow(view: in Slice<byte>, sink: ref Array<byte>, outLen: out int) -> int {",
                        "    outLen = 0",
                        "    return outLen",
                        "}",
                        "export class Box {",
                        "    touch(value: in int, place: ref int, filled: out int) -> ()",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            items = api_inventory.collect_pure_stdlib(root)
            signatures = {
                (item["namespace"], item["name"], item["kind"]): item["signature"]
                for item in items
            }

        self.assertEqual(
            "(view: in Slice<byte>, sink: ref Array<byte>, outLen: out int): int",
            signatures[("modes", "borrow", "function")],
        )
        self.assertEqual(
            "(value: in int, place: ref int, filled: out int): ()",
            signatures[("Box", "touch", "method")],
        )

    def test_pure_stdlib_inventory_keeps_member_call_defaults(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-api-inventory-call-defaults.") as tmp:
            root = Path(tmp)
            module_dir = root / "stdlib" / "process"
            module_dir.mkdir(parents=True)
            (module_dir / "process.xr").write_text(
                "\n".join(
                    [
                        "export class Process {",
                        "    static spawn(program: string, args: Array<string> = Array<string>(0), options: ProcessOptions? = null) -> Process? {",
                        "        return null",
                        "    }",
                        "    configure(args: Array<string> = Array<string>(0)) -> bool {",
                        "        return true",
                        "    }",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            items = api_inventory.collect_pure_stdlib(root)
            signatures = {
                (item["namespace"], item["name"], item["kind"]): item["signature"]
                for item in items
            }
            keys = [(item["namespace"], item["name"], item["kind"]) for item in items]

        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(3, len(items))
        self.assertEqual(
            "(program: string, args: Array<string> = Array<string>(0), options: ProcessOptions? = null): Process?",
            signatures[("Process", "spawn", "static-method")],
        )
        self.assertEqual(
            "(args: Array<string> = Array<string>(0)): bool",
            signatures[("Process", "configure", "method")],
        )

    def test_pure_stdlib_inventory_reads_aligned_structs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-api-inventory-aligned-struct.") as tmp:
            root = Path(tmp)
            module_dir = root / "stdlib" / "vectors"
            module_dir.mkdir(parents=True)
            (module_dir / "vectors.xr").write_text(
                "\n".join(
                    [
                        "export struct U32x4 align(16) {",
                        "    private _lanes: [uint32; 4]",
                        "    static splat(value: uint32) -> U32x4 {",
                        "        return U32x4{_lanes: [value; 4]}",
                        "    }",
                        "    extract(lane: int) -> uint32 {",
                        "        return this._lanes[lane]",
                        "    }",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            items = api_inventory.collect_pure_stdlib(root)
            signatures = {
                (item["namespace"], item["name"], item["kind"]): item["signature"]
                for item in items
            }

        self.assertEqual("U32x4", signatures[("U32x4", "U32x4", "type")])
        self.assertEqual(
            "(value: uint32): U32x4",
            signatures[("U32x4", "splat", "static-method")],
        )
        self.assertEqual(
            "(lane: int): uint32",
            signatures[("U32x4", "extract", "method")],
        )


if __name__ == "__main__":
    unittest.main()
