#!/usr/bin/env python3
"""Encode full Task roles from named fields without a product writer or body copy."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import struct

UNIT, I64, STRING, ERROR, TASK_I64, TASK_STRING = 0, 2, 3, 14, 256, 257
OPS = {"const_int": 2, "slot_load": 4, "copy": 18, "call": 28, "suspend": 29,
       "throw": 30, "return": 33, "local_new": 34, "local_read": 35,
       "invoke_result": 101, "invoke_error": 102, "go": 146, "await": 147}
ROLE_NAMES = ["golden", "scalar_go", "initializer_target", "await_place", "go_target",
              "await_binding", "mutable_worker", "cleanup_helper"]


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def ins(op, result=UNIT, args=(0, 0), targets=(0, 0), immediate=0, type_args=(0, 0)):
    return {"op": OPS[op], "result": result, "args": args, "targets": targets,
            "immediate": immediate, "type_args": type_args}


def function(name, parameters, result, blocks, instructions, operands=()):
    return {"name": name, "parameters": parameters, "result": result, "blocks": blocks,
            "instructions": instructions, "operands": operands}


def encode_function(f):
    name = f["name"].encode("ascii")
    body = words(len(name)) + name + words(len(f["parameters"]), *f["parameters"], f["result"])
    body += words(len(f["blocks"]))
    for block in f["blocks"]:
        body += words(*block)
    body += words(len(f["instructions"]))
    for op in f["instructions"]:
        body += words(op["op"], op["result"], *op["args"], *op["targets"])
        body += struct.pack("<q", op["immediate"]) + words(*op["type_args"])
    return body + words(len(f["operands"]), *f["operands"])


def descriptor_tail(first_element):
    # No generics; two Task descriptors; no nominals/interfaces/defaults/provenance.
    return words(0, 2, 0, 0, 8, 0, first_element, 8, 0, STRING, 0, 0)


def types_body(element):
    f = function("taskTypes", (TASK_I64, TASK_STRING), UNIT, [(0, 1, 0, 0)], [ins("return")])
    return words(0, 1, 0) + encode_function(f) + descriptor_tail(element)


def go_model(role):
    functions = [
        function("worker", (I64,), I64, [(0, 2, 0, 0)], [ins("suspend"), ins("return")]),
        function("root", (), I64, [(0, 6, 0, 0), (6, 2, 0, 0), (8, 2, 0, 0)],
                 [ins("const_int", I64, immediate=7), ins("go", TASK_I64, (0, 1)),
                  ins("copy", TASK_I64, (1, 0)), ins("local_new", TASK_I64, (2, 0)),
                  ins("local_read", TASK_I64, (3, 0)), ins("await", args=(4, 0), targets=(1, 2)),
                  ins("invoke_result", I64, immediate=5), ins("return", args=(6, 0)),
                  ins("invoke_error", ERROR, immediate=5), ins("throw", args=(8, 0))], (0,)),
        function("maker", (I64,), TASK_I64, [(0, 2, 0, 0)],
                 [ins("go", TASK_I64, (0, 1)), ins("return", args=(1, 0))], (0,)),
        function("helper", (I64,), UNIT, [(0, 2, 0, 0)],
                 [ins("call", TASK_I64, (0, 1), immediate=2), ins("return")], (0,)),
        function("unknown", (TASK_I64,), I64, [(0, 1, 0, 0), (1, 2, 0, 0), (3, 2, 0, 0)],
                 [ins("await", targets=(1, 2)), ins("invoke_result", I64), ins("return", args=(2, 0)),
                  ins("invoke_error", ERROR), ins("throw", args=(4, 0))]),
        function("init", (), UNIT, [(0, 1, 0, 0)], [ins("return")])]
    identities = [[0] * 9 for _ in functions]
    slots = []
    root = functions[1]["instructions"]
    if role == "scalar_go":
        root[1]["result"] = I64
    elif role == "initializer_target":
        root[1]["immediate"] = 5
    elif role == "await_place":
        root[5]["args"] = (3, 0)
    elif role == "go_target":
        root[1]["targets"] = (1, 0)
    elif role == "await_binding":
        root[6]["result"] = STRING
    elif role == "mutable_worker":
        functions[0]["instructions"] = [ins("slot_load", I64), ins("return", args=(1, 0))]
        slots = [(0, I64, 1)]
    elif role == "cleanup_helper":
        identities[3][4] = 1
    else:
        assert role == "golden", role
    return {"functions": functions, "identities": identities, "slots": slots,
            "module": {"name": "m", "dependencies": (), "initializer": 5}, "entry": 1, "root": 0}


def go_body(role):
    m = go_model(role)
    body = words(0, len(m["functions"]), 1)
    for f in m["functions"]:
        body += encode_function(f)
    body += words(1, len(m["slots"]), 0, m["root"], m["entry"])
    module = m["module"]
    name = module["name"].encode("ascii")
    body += words(len(name)) + name + words(len(module["dependencies"]), *module["dependencies"], module["initializer"])
    for identity in m["identities"]:
        body += words(*identity)
    for slot in m["slots"]:
        body += words(*slot)
    return body + words(0) + descriptor_tail(I64)


def frame(body, contract):
    prefix = b"XRCHK\0\0\0" + struct.pack("<IIIIQ", 25, contract, 2, 0, len(body))
    return prefix + sha256(prefix + body).digest() + body


def historical(path, name):
    match = re.search(r"static const uint8_t " + name + r"\[\] = \{(.*?)\};",
                      path.read_text(encoding="utf-8"), re.S)
    assert match, name
    return bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))


def render(kind, packets):
    guard = "XIR_TASK_" + kind.upper() + "70_GOLDEN_H"
    lines = ["/* Complete named Task fields and role controls; no product writer is used. */",
             "#ifndef " + guard, "#define " + guard]
    for name, packet in packets:
        lines.append("static const uint8_t " + name + "[] = {")
        lines += ["    " + ",".join("0x%02x" % b for b in packet[i:i + 20]) + ","
                  for i in range(0, len(packet), 20)]
        lines.append("};")
    return "\n".join(lines + ["#endif // " + guard, ""])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    test = args.root / "tests/unit/xir"
    models = {"types": [("golden", types_body(I64))], "unit": [("golden", types_body(UNIT))],
              "go": [(role, go_body(role)) for role in ROLE_NAMES]}
    rows = []
    for kind, entries in models.items():
        for role, body in entries:
            old_contracts = [67, 68, 69] if kind == "unit" else [66, 67, 68, 69]
            for contract in old_contracts:
                name = "task_" + kind + str(contract) + "_" + role
                previous = historical(test / ("xir_task_" + kind + str(contract) + "_golden.h"), name)
                assert previous == frame(body, contract), name
            data = frame(body, 70)
            rows.append({"kind": kind, "role": role, "bytes": len(data), "body_sha256": sha256(body).hexdigest(),
                         "current_sha256": sha256(data).hexdigest(), "old_complete_models_exact": old_contracts,
                         "callable_descriptors": 0, "body_reused_from_writer": False})
        content = render(kind, [("task_" + kind + "70_" + role, frame(body, 70)) for role, body in entries])
        target = (args.output_dir or test) / ("xir_task_" + kind + "70_golden.h")
        if args.output_dir:
            target.write_text(content, encoding="utf-8", newline="\n")
        else:
            assert target.read_text(encoding="utf-8") == content
    print(json.dumps({"status": "INDEPENDENT_NAMED_MODEL_EXACT_NOT_PRODUCT_TEST", "rows": rows}, indent=2))


if __name__ == "__main__":
    main()
