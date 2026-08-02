#!/usr/bin/env python3
"""Focused contracts for the source-derived hosted-fragment generator."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
GENERATOR = ROOT / "tools" / "stdlibgen" / "generate_vm_fastpaths.py"
SPEC = importlib.util.spec_from_file_location("xray_generate_vm_fastpaths", GENERATOR)
assert SPEC and SPEC.loader
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)


class HostedSignatureTest(unittest.TestCase):
    def test_callback_arrow_does_not_hide_the_final_parameter(self) -> None:
        value_types = dict(generator.VALUE_TYPES)
        self.assertIsNone(
            generator.hosted_signature(
                "(method: string, handler: (Request) -> Response): ()", value_types
            )
        )

    def test_nested_generic_commas_remain_inside_one_parameter(self) -> None:
        value_types = dict(generator.VALUE_TYPES)
        value_types["Map<string, Array<int>>"] = (
            "map",
            "XrValue",
            "",
            "",
            "",
        )
        self.assertEqual(
            ([("p0", "Map<string, Array<int>>", None), ("p1", "bool", None)], "int"),
            generator.hosted_signature(
                "(values: Map<string, Array<int>>, strict: bool): int", value_types
            ),
        )

    def test_source_owned_default_expressions_are_preserved(self) -> None:
        value_types = dict(generator.VALUE_TYPES)
        value_types["Options"] = ("object:test:Options", "XrValue", "", "", "")
        self.assertEqual(
            (
                [
                    ("p0", "string", '"a,b"'),
                    ("p1", "Options", "Options()"),
                ],
                "int",
            ),
            generator.hosted_signature(
                '(name: string = "a,b", options: Options = Options()): int',
                value_types,
            ),
        )

        entry = {
            "params": [("p0", "string", '"a,b"'), ("p1", "Options", "Options()")]
        }
        self.assertEqual(
            [
                ("_provided", "int", None),
                ("p0", "string?", None),
                ("p1", "Options?", None),
            ],
            generator.hosted_bridge_params(entry),
        )
    def test_incomplete_class_is_removed_atomically(self) -> None:
        entries = [
            {
                "symbol": "sync.Once.constructor.constructor",
                "module": "sync",
                "class_name": "Once",
                "reference": "stdlib/sync/sync.xr::Once.constructor",
                "abi": "unit->object:sync:Once",
            },
            {
                "symbol": "text.trim",
                "module": "text",
                "class_name": "",
                "reference": "stdlib/text/text.xr::trim",
                "abi": "string->string",
            },
            {
                "symbol": "http.textResponse",
                "module": "http",
                "class_name": "",
                "reference": "stdlib/http/http.xr::textResponse",
                "abi": "string->object:http:HttpResponse",
            },
            {
                "symbol": "http.HttpResponse.constructor.constructor",
                "module": "http",
                "class_name": "HttpResponse",
                "reference": "stdlib/http/http.xr::HttpResponse.constructor",
                "abi": "unit->object:http:HttpResponse",
            },
        ]
        source_exports = [
            {
                "symbol": "sync.Once.constructor.constructor",
                "module": "sync",
                "class_name": "Once",
                "source": "stdlib/sync/sync.xr",
                "signature": "(): ()",
            },
            {
                "symbol": "sync.Once.method.call",
                "module": "sync",
                "class_name": "Once",
                "source": "stdlib/sync/sync.xr",
                "signature": "(body: () -> ()): ()",
            },
            {
                "symbol": "text.trim",
                "module": "text",
                "class_name": "",
                "source": "stdlib/text/text.xr",
                "signature": "(value: string): string",
            },
            {
                "symbol": "http.textResponse",
                "module": "http",
                "class_name": "",
                "source": "stdlib/http/http.xr",
                "signature": "(value: string): HttpResponse",
            },
            {
                "symbol": "http.HttpResponse.constructor.constructor",
                "module": "http",
                "class_name": "HttpResponse",
                "source": "stdlib/http/http.xr",
                "signature": "(): ()",
            },
            {
                "symbol": "http.HttpResponse.method.json",
                "module": "http",
                "class_name": "HttpResponse",
                "source": "stdlib/http/http.xr",
                "signature": "(): Json",
            },
        ]
        filtered, unsupported = generator.enforce_atomic_class_coverage(
            entries, source_exports
        )
        self.assertEqual(["text.trim"], [entry["symbol"] for entry in filtered])
        self.assertEqual(
            [
                "http.HttpResponse.constructor.constructor",
                "http.HttpResponse.method.json",
                "http.textResponse",
                "sync.Once.constructor.constructor",
                "sync.Once.method.call",
            ],
            [entry["symbol"] for entry in unsupported],
        )

    def test_registry_requires_generated_suspendability_header(self) -> None:
        registry = generator.render_registry(1, [], "fingerprint", "test-target")
        self.assertIn("#ifndef XR_HOSTED_FRAGMENT_SUSPENDABILITY_DECLARED", registry)
        self.assertIn(
            '#error "include the generated hosted-fragment header before this registry"',
            registry,
        )


if __name__ == "__main__":
    unittest.main()
