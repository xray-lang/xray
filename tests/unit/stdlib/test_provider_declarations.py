#!/usr/bin/env python3
"""Semantic declarations never inherit facts from a provider ABI or symbol."""

from dataclasses import replace
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/stdlibgen"))
from provider_declarations import (PIPE_RESOURCE, PROPERTIES, admission_reasons,
                                   declaration_inventory, parse_provider_declaration)


def properties():
    return {
        "provider_contract": "xray.runtime.provider.v1/clock",
        "provider_operation": "xray.runtime.provider-operation.v1/clock/realtime-nanos",
        "provider_parameter_modes": "", "provider_parameter_owners": "",
        "provider_result_owner": "trivial", "provider_error": "none", "provider_panic": "none",
        "provider_suspend": "never", "provider_refusal": "trap",
        "provider_resources": "[]", "provider_effects": "reads-clock",
        "provider_threads": "any", "provider_reentry": "allowed",
        "provider_callbacks": "none", "provider_platforms": "linux,macos,windows",
        "provider_profiles": "hosted", "provider_adapter": "i64-nullary-u64",
        "provider_host_header": "os/os_time.h", "provider_host_symbol": "xr_time_realtime_ns",
    }


def parse(props=None, parameters=(), result="i64"):
    return parse_provider_declaration(properties() if props is None else props,
                                      parameters, result, "sample.__probe")


class ProviderDeclarationTests(unittest.TestCase):
    def test_repository_inventory_has_every_private_leaf(self):
        from stdlibgen import parse_defs
        inventory = declaration_inventory(parse_defs(ROOT))
        self.assertEqual(153, inventory["leaf_count"])
        self.assertEqual(11, sum(row["declared"] for row in inventory["leaves"]))
        self.assertEqual(11, sum(row["admitted"] for row in inventory["leaves"]))
        generated = json.loads((ROOT / "stdlib/provider_inventory.generated.json").read_text())
        self.assertEqual(json.loads(json.dumps(inventory)), generated)

    def test_every_fact_is_explicit_and_unknown_facts_are_rejected(self):
        self.assertEqual(set(properties()), PROPERTIES)
        for key in PROPERTIES:
            with self.subTest(missing=key):
                props = properties()
                del props[key]
                with self.assertRaisesRegex(SystemExit, "missing explicit provider facts"):
                    parse(props)
        with self.assertRaisesRegex(SystemExit, "unknown provider properties"):
            parse({**properties(), "provider_thread": "any"})
        self.assertIsNone(parse_provider_declaration({}, (), "i64", "ordinary"))

    def test_explicit_semantics_are_separate_from_host_projection(self):
        declaration = parse()
        self.assertFalse(admission_reasons(declaration))
        changed_host = parse({**properties(), "provider_host_symbol": "another_implementation"})
        self.assertEqual(declaration.logical.declaration_sha256(),
                         changed_host.logical.declaration_sha256())
        for field, value in [("threads", "instance-affine"), ("reentry", "forbidden"),
                             ("error", "typed"), ("panic", "may-panic"),
                             ("suspend", "may-suspend"), ("callbacks", "synchronous")]:
            changed = replace(declaration, logical=replace(declaration.logical, **{field: value}))
            with self.subTest(field=field):
                self.assertNotEqual(declaration.logical.declaration_sha256(),
                                    changed.logical.declaration_sha256())
                self.assertTrue(admission_reasons(changed))
        changed = replace(declaration.logical, effects=("reads-process",))
        self.assertNotEqual(declaration.logical.declaration_sha256(), changed.declaration_sha256())

    def test_signature_mismatch_is_not_repaired_from_the_host_symbol(self):
        declaration = parse(result="bool")
        self.assertIn("declared-signature-does-not-match-explicit-host-adapter",
                      admission_reasons(declaration))
        with self.assertRaisesRegex(SystemExit, "every logical parameter"):
            parse(parameters=("i64",))

    def test_typed_ownership_is_recursive_and_not_inferred_from_host(self):
        props = {**properties(), "provider_adapter": "typed", "provider_result_owner": "owned",
                 "provider_parameter_modes": "in", "provider_parameter_owners": "borrow"}
        declaration = parse(props, ("(i64, string)?",), "resource<mem.__BufferStorage>")
        self.assertFalse(admission_reasons(declaration))
        for logical in (replace(declaration.logical, result_owner="trivial"),
                        replace(declaration.logical, parameters=(replace(
                            declaration.logical.parameters[0], owner="trivial"),)),
                        replace(declaration.logical, parameters=(replace(
                            declaration.logical.parameters[0], mode="move"),))):
            with self.subTest(logical=logical):
                self.assertIn("typed-signature-ownership-mismatch",
                              admission_reasons(replace(declaration, logical=logical)))
        too_large = replace(declaration.logical, result_type="(" + ",".join(
            ["resource<mem.__BufferStorage>"] * 4) + ")")
        self.assertIn("logical-signature-exceeds-transport-bound",
                      admission_reasons(replace(declaration, logical=too_large)))

    def test_explicit_provider_error_contract_publishes_analyzer_fact(self):
        from stdlibgen import parse_defs
        props = {"signature": "(): i64", "doc": "probe", "vm": "sample_probe",
                 "argc": 0, "arg_spec": "", "visibility": "internal", **properties()}
        with tempfile.TemporaryDirectory(prefix="xray-provider-error.") as tmp:
            root = Path(tmp)
            definitions = root / "stdlib" / "defs"
            definitions.mkdir(parents=True)
            for effect in (None, "nothrow", "may-error"):
                current = dict(props)
                if effect is not None:
                    current["effect"] = effect
                text = "module sample {\n  fn __probe {\n" + "".join(
                    f"    {key}: {json.dumps(value)}\n" for key, value in current.items()) + "  }\n}\n"
                (definitions / "core.def").write_text(text, encoding="utf-8")
                if effect == "may-error":
                    with self.assertRaisesRegex(SystemExit, "contradicts effect"):
                        parse_defs(root)
                else:
                    entries = parse_defs(root)
                    self.assertEqual(1, len(entries))
                    self.assertEqual("nothrow", entries[0].effect)

    def test_set_order_is_canonical_but_duplicates_are_rejected(self):
        self.assertEqual(parse().logical.declaration_sha256(),
                         parse({**properties(), "provider_platforms": "windows,linux,macos"})
                         .logical.declaration_sha256())
        with self.assertRaisesRegex(SystemExit, "distinct supported values"):
            parse({**properties(), "provider_platforms": "linux,linux"})
        with self.assertRaisesRegex(SystemExit, "cannot combine none"):
            parse({**properties(), "provider_effects": "none,reads-clock"})

    def test_resource_duplicate_fields_cannot_replace_semantic_facts(self):
        raw = '[{"resource":"xray.runtime.resource.v1/pipe-endpoint", "value":"result.some.0",'
        raw += '"action":"acquire", "when":"call-enter", "when":"result-present"}]'
        with self.assertRaisesRegex(SystemExit, "duplicate resource field when"):
            parse({**properties(), "provider_resources": raw}, result="(i64, i64)?")

    def test_pipe_acquisition_requires_both_conditional_result_tokens(self):
        resources = [{"resource": PIPE_RESOURCE, "value": f"result.some.{index}",
                      "action": "acquire", "when": "result-present"} for index in (0, 1)]
        props = {**properties(), "provider_adapter": "optional-i64-pair-pipe-create",
                 "provider_resources": json.dumps(resources)}
        self.assertFalse(admission_reasons(parse(props, result="(i64, i64)?")))
        props["provider_resources"] = json.dumps(resources[:1])
        self.assertIn("declared-resources-do-not-match-explicit-host-adapter",
                      admission_reasons(parse(props, result="(i64, i64)?")))
        resources[0]["when"] = "call-enter"
        props["provider_resources"] = json.dumps(resources)
        with self.assertRaisesRegex(SystemExit, "invalid result token transition"):
            parse(props, result="(i64, i64)?")

    def test_false_close_consumes_token_on_call_entry(self):
        transition = {"resource": PIPE_RESOURCE, "value": "parameter.0",
                      "action": "consume", "when": "call-enter"}
        props = {**properties(), "provider_parameter_modes": "in",
                 "provider_parameter_owners": "trivial", "provider_adapter": "bool-i64-pipe-close",
                 "provider_resources": json.dumps([transition])}
        declaration = parse(props, ("i64",), "bool")
        self.assertFalse(admission_reasons(declaration))
        self.assertEqual("trap", declaration.logical.refusal)
        transition["when"] = "result-true"
        with self.assertRaisesRegex(SystemExit, "invalid parameter token transition"):
            parse({**props, "provider_resources": json.dumps([transition])}, ("i64",), "bool")

    def test_inventory_retains_undeclared_and_unadmitted_leaves(self):
        def entry(name, declaration):
            return SimpleNamespace(is_internal=True, name=name, symbol="sample." + name,
                                   signature="(): i64", provider_declaration=declaration)
        rows = [entry("__available", parse()), entry("__missing", None),
                entry("__future", parse({**properties(), "provider_adapter": "future-adapter",
                                         "provider_operation":
                                         "xray.runtime.provider-operation.v1/clock/future"}))]
        inventory = declaration_inventory(rows)
        self.assertEqual(3, inventory["leaf_count"])
        states = {row["symbol"]: (row["declared"], row["admitted"])
                  for row in inventory["leaves"]}
        self.assertEqual({"sample.__available": (True, True), "sample.__missing": (False, False),
                          "sample.__future": (True, False)}, states)
        self.assertTrue(all(row["binding"]["state"] == "NOT_VERIFIED"
                            and row["testing"]["state"] == "NOT_VERIFIED"
                            for row in inventory["leaves"]))
        rows.append(entry("__duplicate", parse()))
        with self.assertRaisesRegex(SystemExit, "duplicate provider operation identity"):
            declaration_inventory(rows)


if __name__ == "__main__":
    unittest.main()
