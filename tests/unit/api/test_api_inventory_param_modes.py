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
            "(a: int, b: ref string, c: move Buffer): ()",
            api_inventory.normalize_signature(
                " a: int,\n    b: ref string,\n    c: move Buffer ", None
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
                        "export fn transfer(view: Slice<byte>, sink: ref Array<byte>, job: move Buffer) -> int {",
                        "    sink.push(len(view))",
                        "    return len(view)",
                        "}",
                        "export class Box {",
                        "    touch(value: int, place: ref int, job: move Buffer) -> ()",
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
            "(view: Slice<byte>, sink: ref Array<byte>, job: move Buffer): int",
            signatures[("modes", "transfer", "function")],
        )
        self.assertEqual(
            "(value: int, place: ref int, job: move Buffer): ()",
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

    def test_pure_stdlib_inventory_joins_multiline_member_signatures(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-api-inventory-multiline.") as tmp:
            root = Path(tmp)
            module_dir = root / "stdlib" / "jobs"
            module_dir.mkdir(parents=True)
            (module_dir / "jobs.xr").write_text(
                "\n".join(
                    [
                        "export class Job {",
                        "    constructor(name: string, retries: int,",
                        "                run: (int) -> string) {",
                        "        this.name = name",
                        "    }",
                        "    static create(name: string,",
                        "                  run: (int) -> string) -> Job {",
                        "        return Job(name, 0, run)",
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

        self.assertEqual(
            "(name: string, retries: int, run: (int) -> string): ()",
            signatures[("Job", "constructor", "method")],
        )
        self.assertEqual(
            "(name: string, run: (int) -> string): Job",
            signatures[("Job", "create", "static-method")],
        )

    def test_pure_stdlib_inventory_joins_multiline_top_level_signatures(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-api-inventory-top-multiline.") as tmp:
            root = Path(tmp)
            module_dir = root / "stdlib" / "jobs"
            module_dir.mkdir(parents=True)
            (module_dir / "jobs.xr").write_text(
                "\n".join(
                    [
                        "export fn schedule(name: string,",
                        "                   run: (int) -> string) -> string {",
                        "    return run(1)",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            items = api_inventory.collect_pure_stdlib(root)

        self.assertEqual(1, len(items))
        self.assertEqual("schedule", items[0]["name"])
        self.assertEqual(
            "(name: string, run: (int) -> string): string", items[0]["signature"]
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
                        "    private _lanes: [u32; 4]",
                        "    static splat(value: u32) -> U32x4 {",
                        "        return U32x4{_lanes: [value; 4]}",
                        "    }",
                        "    extract(lane: int) -> u32 {",
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
            "(value: u32): U32x4",
            signatures[("U32x4", "splat", "static-method")],
        )
        self.assertEqual(
            "(lane: int): u32",
            signatures[("U32x4", "extract", "method")],
        )

    def test_pure_stdlib_inventory_reads_exported_type_alias_fields(self) -> None:
        with tempfile.TemporaryDirectory(prefix="xray-api-inventory-type-alias.") as tmp:
            root = Path(tmp)
            module_dir = root / "stdlib" / "cluster"
            module_dir.mkdir(parents=True)
            (module_dir / "cluster.xr").write_text(
                "\n".join(
                    [
                        "export type Config = {",
                        "    name: string,",
                        "    capacity: int,",
                        "}",
                        "",
                    ]
                ),
                encoding="utf-8",
            )

            items = api_inventory.collect_pure_stdlib(root)
            signatures = {
                (entry["namespace"], entry["name"], entry["kind"]): entry["signature"]
                for entry in items
            }

        self.assertEqual("{ name: string, capacity: int, }",
                         signatures[("cluster", "Config", "type")])
        self.assertEqual("string", signatures[("cluster", "Config.name", "field")])
        self.assertEqual("int", signatures[("cluster", "Config.capacity", "field")])


if __name__ == "__main__":
    unittest.main()
