"""Derive scalar source-module closure identity from explicit source fields."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from derive_identity_vectors import digest, enum_values, fixed_array, frame, module, source_begin, u32, u64


ENTRY_SOURCE = 'import { add1 } from "./producer"\nfn root() -> i64 { return add1(41) }\n'
PRODUCER_SOURCE = 'export fn add1(value: i64) -> i64 { return value + 1 }\n'


def derive(schema: int, enums: dict[str, int]) -> dict:
    def begin(domain: str) -> bytes:
        return frame(domain.encode()) + u32(schema)

    def locator(kind: str, line: int, text: str, first: str, last: str | None = None) -> bytes:
        start = text.index(first) + 1
        end = len(text) + 1 if last is None else text.index(last) + len(last) + 1
        return b"".join(u32(value) for value in (enums[kind], line, start, line, end))

    entry_text = ENTRY_SOURCE.splitlines()[1]
    producer_text = PRODUCER_SOURCE.strip()
    import_text = ENTRY_SOURCE.splitlines()[0]
    entry_span = locator("AST_FUNCTION_DECL", 2, entry_text, "root")
    export_span = locator("AST_FUNCTION_DECL", 1, producer_text, "add1")
    import_span = locator("AST_IMPORT_STMT", 1, import_text, "import")
    call_span = locator("AST_CALL_EXPR", 2, entry_text, "add1", "add1(41)")
    entry = module("module-id-v1:kind=6:script:namespace=0::path=8:entry.xr", ENTRY_SOURCE)
    producer = module("module-id-v1:kind=6:script:namespace=0::path=11:producer.xr", PRODUCER_SOURCE)
    entry["source"] = digest(b"xray-module-source-v1\0" + frame(ENTRY_SOURCE.encode()))
    producer["source"] = digest(b"xray-module-source-v1\0" + frame(PRODUCER_SOURCE.encode()))
    policy = digest(begin("xray-source-scalar-module-graph-policy-v1") +
                    b"".join(u32(value) for value in (2, 1, 1, 0, 2, 1, 1)))
    effect = digest(source_begin("xray-language-scalar-effect-contract-v1") + bytes(36))
    signatures = []
    for count in (0, 1):
        payload = source_begin("xray-language-scalar-function-signature-v1") + u32(count) * 2 + bytes([0])
        if count:
            payload += u32(4) + bytes([0])
        signatures.append(digest(payload + u32(4)))
    call_contract = digest(source_begin("xray-source-scalar-call-contract-v1") +
                           frame(signatures[1]) + frame(effect) + u64(0))
    scalar_owner = digest(b"xray-exact-scalar-registry-authority-v1\0" + u32(schema))[:16]
    scalar_decl = digest(b"xray-exact-scalar-declaration-v1\0" + u32(4))[:16]
    scalar_instance = digest(b"xray-exact-scalar-instance-v1\0" + scalar_decl + u32(4))[:16]
    shape = digest(b"xray-program-semantic-typed-shape-v1\0" +
                   b"".join(u32(value) for value in (schema, 1, 4, 15, 0)))
    ownership = digest(b"xray-program-semantic-leaf-ownership-v1\0" +
                       b"".join(u32(value) for value in (schema, 1, 15, 1, 1, 0, 0, 0)))
    scalar = digest(b"xray-program-exact-scalar-type-v1\0" + u32(schema) + u32(4) + shape + ownership)[:16]
    functions = []
    for owner, span, signature, flag, count in (
        (entry, entry_span, signatures[0], 1, 0), (producer, export_span, signatures[1], 2, 1)
    ):
        declaration = digest(begin("xray-source-scalar-graph-function-declaration-v1") +
                             frame(owner["id"]) + frame(owner["source"]) + span + frame(signature))[:16]
        instance = digest(begin("xray-source-scalar-graph-function-instance-v1") +
                          frame(declaration) + frame(signature))[:16]
        identifier = digest(b"xray-program-semantic-function-v1\0" + u32(1) + policy +
                            owner["id"] + declaration + instance + signature + effect + u64(0))[:16]
        functions.append({"id": identifier, "module": owner["id"], "declaration": declaration,
                          "instance": instance, "span": span, "signature": signature,
                          "flags": flag, "count": count})
    entry_function, exported_function = functions
    producer["exports"] = digest(begin("xray-source-scalar-module-export-v1") +
                                 frame(producer["id"]) + frame(producer["authority"]) +
                                 frame(producer["source"]) + frame(exported_function["declaration"]) +
                                 frame(exported_function["id"]) + frame(signatures[1]) + frame(effect) +
                                 u64(0) + u32(4) + u32(1) + u32(0))
    modules = (entry, producer)
    framed_modules = b"".join(frame(owner[key]) for owner in modules
                               for key in ("id", "authority", "source", "exports"))
    export_frames = frame(exported_function["declaration"]) + frame(exported_function["id"])
    signature_frames = frame(scalar) + frame(signatures[1]) + frame(effect) + u64(0) + u32(1) + u32(0)
    resolver = digest(begin("xray-source-scalar-graph-resolver-binding-v1") + framed_modules +
                      u32(1) + import_span + export_frames + signature_frames)[:16]
    dependency = digest(begin("xray-source-scalar-graph-dependency-contract-v2") + framed_modules +
                        import_span + export_frames + frame(resolver) + signature_frames)
    callsite = digest(source_begin("xray-source-program-callsite-v2") + frame(entry["source"]) +
                      frame(entry["id"]) + frame(entry_function["declaration"]) + call_span)[:16]
    call = digest(b"xray-program-semantic-call-v2\0" + u32(2) + policy + callsite +
                  entry_function["id"] + exported_function["id"] + resolver + call_contract)[:16]
    payload = b"xray-program-semantic-closure-v9\0" + u32(schema) + u32(4) + policy
    payload += b"".join(u32(value) for value in (2, 1, 1, 0, 2, 1, 1))
    for owner in sorted(modules, key=lambda owner: owner["id"]):
        payload += b"".join(owner[key] for key in ("id", "authority", "source", "exports"))
    payload += entry["id"] + producer["id"] + import_span
    payload += exported_function["declaration"] + exported_function["id"] + resolver + dependency + u32(1)
    payload += scalar + scalar_owner + scalar_decl + scalar_instance + bytes(20) + shape + ownership
    payload += b"".join(u32(value) for value in (0, 0, 1, 4, 15))
    parameter = 0
    for row in sorted(functions, key=lambda row: row["id"]):
        payload += row["id"] + row["module"] + row["declaration"] + row["instance"] + row["span"]
        payload += row["signature"] + effect + scalar + u32(parameter) + u32(row["count"]) + u64(0)
        payload += bytes([row["flags"]])
        parameter += row["count"]
    payload += exported_function["id"] + scalar + u32(0) + u32(0)
    payload += call + callsite + call_span + entry_function["id"] + exported_function["id"] + resolver + call_contract
    fingerprint = digest(payload)
    generation = digest(b"xray-generation-closure-id-v1\0" + u32(schema) + fingerprint)[:16]

    def hexadecimal(value):
        if isinstance(value, bytes):
            return value.hex()
        if isinstance(value, dict):
            return {key: hexadecimal(part) for key, part in value.items()}
        if isinstance(value, (list, tuple)):
            return [hexadecimal(part) for part in value]
        return value

    return hexadecimal({"schema": schema, "modules": modules, "functions": functions,
                        "entry_source": ENTRY_SOURCE, "producer_source": PRODUCER_SOURCE,
                        "policy": policy, "effect": effect, "signatures": signatures,
                        "scalar": scalar, "scalar_owner": scalar_owner, "scalar_decl": scalar_decl,
                        "scalar_instance": scalar_instance, "shape": shape, "ownership": ownership,
                        "import_span": import_span, "call_span": call_span, "resolver": resolver,
                        "dependency": dependency, "call_contract": call_contract,
                        "callsite": callsite, "call": call, "framed_bytes": payload,
                        "fingerprint": fingerprint, "generation": generation})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    enums = enum_values((root / "src/frontend/parser/xast_types.h").read_text(encoding="utf-8"))
    closure_header = (root / "src/plan/semantic/xr_program_semantic_closure.h").read_text(encoding="utf-8")
    assert "XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION UINT32_C(10)" in closure_header
    old = derive(9, enums)
    print(json.dumps({key: old[key] for key in ("import_span", "call_span", "resolver", "fingerprint", "generation")}, indent=2))
    assert old["resolver"] == "7ce03d6c28e4661bdc92c76b264ee2af"
    assert old["fingerprint"] == "8b4be9036111974b5e71ce164342fa5d319221f307f400829470000644456c27"
    assert old["generation"] == "620ef1566d11dc7839aa7fc800f4d527"
    current = derive(10, enums)
    test_text = (root / "tests/unit/frontend/test_xa_program_semantic_closure.c").read_text(encoding="utf-8")
    for name, key in (("expected_resolver_binding", "resolver"),
                      ("expected_fingerprint", "fingerprint"),
                      ("expected_generation_id", "generation")):
        assert fixed_array(test_text, name).hex() == current[key]
        assert fixed_array(test_text, name + "_v9").hex() == old[key]
    args.output.write_text(json.dumps({"closure9": old, "closure10": current}, indent=2) + "\n", encoding="utf-8")
    print("PASS legacy scalar source graph: resolver, all rows, fingerprint, generation")
    print("PASS current and obsolete C identity vectors match independent full-field derivation")
    print(json.dumps({key: current[key] for key in ("resolver", "fingerprint", "generation")}, indent=2))


if __name__ == "__main__":
    main()
