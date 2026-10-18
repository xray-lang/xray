from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
mode = sys.argv[3]
assert mode in ("--normal", "--resources", "--before")
env = {**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())}
maximum = str((1 << 64) - 1)
stats_re = re.compile(rb"SIGNATURE source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) created=1 producer_dead=1 physical0/0 JSON0/0 app0/0")
callback = lambda args, result: f"fn: fn({args}) -> {result} [root: UNRESOLVED]"
shapes = {
    25: ("get", ["index: i64"], "i64", 0),
    26: ("set", ["index: i64", "value: i64"], "()", 1),
    27: ("withCapacity", ["capacity: i64"], "Array<i64>", 0),
    28: ("reserve", ["capacity: i64"], "Array<i64>", 0),
    29: ("push", ["value: i64"], "()", 0),
    30: ("join", ["separator?: string"], "string", 0),
    31: ("concat", ["...arrays: Array<i64>"], "Array<i64>", 0),
    32: ("clear", [], "()", None),
    33: ("reverse", [], "Array<i64>", None),
    34: ("unshift", ["value: i64"], "()", 0),
    35: ("pop", [], "i64?", None),
    36: ("shift", [], "i64?", None),
    37: ("resize", ["length: i64", "fill: i64"], "Array<i64>", 1),
    38: ("fill", ["value: i64", "start?: i64", "end?: i64"], "Array<i64>", 2),
    39: ("fill", ["value: i64", "start?: i64", "end?: i64"], "Array<i64>", 0),
    40: ("entries", [], "Array<(i64, i64)>", None),
    41: ("map", [callback("i64", "string")], "Array<string>", 0),
    42: ("reduce", [callback("i64, i64", "i64"), "initial: i64"], "i64", 1),
    43: ("filter", [callback("i64", "bool")], "Array<i64>", 0),
    44: ("forEach", [callback("i64", "()")], "()", 0),
    45: ("find", [callback("i64", "bool")], "i64?", 0),
    46: ("findIndex", [callback("i64", "bool")], "i64", 0),
    47: ("every", [callback("i64", "bool")], "bool", 0),
    48: ("some", [callback("i64", "bool")], "bool", 0),
    49: ("indexOf", ["value: i64"], "i64", 0),
    50: ("contains", ["value: i64"], "bool", 0),
    52: ("get", ["index: i64"], "Item", 0),
    53: ("push", ["value: i16"], "()", 0),
    57: ("get", ["index: i64"], "rune", 0),
    58: ("pop", [], "(fn(i64) -> i64 [root: UNRESOLVED])?", None),
}
for n in (51, 55, 56, 59, 60):
    shapes[n] = shapes[25]
shapes[54] = shapes[30]

def expected(scenario):
    name, parameters, result, active = shapes[scenario]
    element = "Item" if scenario == 52 else "i16" if scenario == 53 else "rune" if scenario == 57 else "fn(i64) -> i64 [root: UNRESOLVED]" if scenario == 58 else "i64"
    value = {"signatures": [{"label": f"fn Array<{element}>.{name}({', '.join(parameters)}) -> {result}",
        "parameters": [{"label": parameter} for parameter in parameters]}], "activeSignature": 0}
    if active is not None:
        value["activeParameter"] = active
    return value

def unpack(data):
    messages = []
    while data:
        header, separator, rest = data.partition(b"\r\n\r\n")
        assert separator and re.fullmatch(rb"Content-Length: [0-9]+", header)
        size = int(header.split(b": ")[1])
        assert 0 < size <= len(rest)
        messages.append(json.loads(rest[:size]))
        data = rest[size:]
    return messages

with tempfile.TemporaryDirectory(prefix="native-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="native"\nmain="root.xr"\n', encoding="utf-8")
    uri, library = [(root / f"{name}.xr").as_uri() for name in ("root", "lib")]
    def invoke(*args):
        result = subprocess.run([str(exe.resolve()), uri, "--signature", library, *map(str, args)],
            input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=290 if mode == "--resources" else 20)
        assert result.returncode == 0, (args, result.returncode, result.stderr[-5000:])
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
        return result
    if mode == "--before":
        for scenario in (25, 29, 40, 52):
            messages = unpack(invoke(scenario).stdout)
            assert len(messages) == 1 and messages[0]["error"] == {"code": -32603, "message": "Source query failed (stage=7 status=11)"}, messages
        print("Fixed consumer source rejects four checked native call signatures; physical0/0")
        sys.exit(0)
    rows = []
    if mode == "--normal":
        for scenario in range(25, 61):
            result = invoke(scenario)
            if scenario in (55, 56):
                value = json.loads(result.stdout)
                assert (b"SIGNATURE_SYNC actual_public_handler=1 Source_owner_dead=1" if scenario == 55 else b"SIGNATURE_DEATH server_poisoned=1 one_shot=1") in result.stderr
            else:
                messages = unpack(result.stdout)
                assert len(messages) == 1 and messages[0]["id"] == "source\0λ" and "result" in messages[0], (scenario, messages)
                value = messages[0]["result"]
            assert value == expected(scenario), (scenario, value, expected(scenario))
            rows.append({"scenario": scenario, "passed": True})
    variants = (int(sys.argv[4]),) if mode == "--resources" else (27, 31, 38, 40, 41, 42, 52, 53, 58, 59, 60)
    measured = []
    for variant in variants:
        result = invoke("--resource", maximum, maximum, variant)
        match = stats_re.search(result.stderr)
        assert match and all(int(match[n]) > 0 for n in (1, 2, 6, 7, 8)) and all(int(match[n]) == 0 for n in (3, 4, 5)), result.stderr[-3000:]
        messages = unpack(result.stdout)
        assert len(messages) == 1 and messages[0].get("result") == expected(variant), messages
        measured.append(dict(variant=variant, source=int(match[1]), json=int(match[2]), allocated=int(match[6]), peak=int(match[7]), work=int(match[8])))
    if mode == "--resources":
        m = measured[0]
        chunks = [(domain, start, min(64, m[domain] - start)) for domain in ("source", "json") for start in range(0, m[domain], 64)]
        def run_chunk(chunk):
            domain, start, count = chunk
            result = invoke("--chunk", domain, start, count, m[domain], m["variant"])
            assert f"SIGNATURE_CHUNK domain={domain} start={start} count={count} total={m[domain]} checked={count} physical0/0 JSON0/0 app0/0".encode() in result.stderr
            for message in unpack(result.stdout):
                assert "result" not in message and message["error"]["code"] == -32603, message
            return dict(domain=domain, start=start, count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:
            rows.extend(f.result() for f in as_completed([pool.submit(run_chunk, chunk) for chunk in chunks]))
        result = invoke("--axes", m["allocated"], m["peak"], m["work"], m["variant"])
        assert b"SIGNATURE_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0" in result.stderr
        messages = unpack(result.stdout)
        assert sum("result" in message for message in messages) == 6, messages
        for message in messages:
            assert message.get("result") == expected(m["variant"]) if "result" in message else message["error"]["code"] == -32603
    print(json.dumps(dict(mode=mode, image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), measured=measured,
        source_total=sum(m["source"] for m in measured), json_total=sum(m["json"] for m in measured), full_FI=mode == "--resources", rows=rows)))
