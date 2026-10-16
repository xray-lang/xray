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
assert mode in ("--normal", "--resources", "--counterfactual", "--generic-resources")
env = {**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())}
maximum = str((1 << 64) - 1)
stats_re = re.compile(rb"SIGNATURE source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) created=1 producer_dead=1 physical0/0 JSON0/0 app0/0")

def unpack(data):
    messages = []
    while data:
        head, separator, rest = data.partition(b"\r\n\r\n")
        assert separator and re.fullmatch(rb"Content-Length: [0-9]+", head)
        size = int(head.split(b": ")[1])
        assert 0 < size <= len(rest)
        messages.append(json.loads(rest[:size]))
        data = rest[size:]
    return messages

def check_signature(value, variant=0):
    if variant == 12:
        assert value is None, value
        return
    if variant == 17:
        assert value == {"signatures": [{"label": "fn Box.add(other: i64) -> i64", "parameters": [{"label": "other: i64"}]}], "activeSignature": 0, "activeParameter": 0}, value
        return
    if variant == 20:
        assert value == {"signatures": [{"label": "fn Box<i64>.add(other: i64) -> i64", "parameters": [{"label": "other: i64"}]}], "activeSignature": 0, "activeParameter": 0}, value
        return
    if variant == 21:
        assert value == {"signatures": [{"label": "fn Box.pick(value: i64) -> i64", "parameters": [{"label": "value: i64"}]}], "activeSignature": 0, "activeParameter": 0}, value
        return
    shown_type = "Item" if variant == 24 else "i64"
    parameters = [] if variant == 11 else ["left: ref i64" if variant == 10 else f"left: {shown_type}", f"right: {shown_type}"]
    name = "value" if variant == 11 else "mix"
    result_type = "(i64, i64)" if variant == 22 else shown_type
    expected = {"signatures": [{"label": f"fn {name}({', '.join(parameters)}) -> {result_type}", "parameters": [{"label": p} for p in parameters]}], "activeSignature": 0}
    if parameters:
        expected["activeParameter"] = 1
    assert value == expected, (value, expected)

with tempfile.TemporaryDirectory(prefix="signature-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="signature"\nmain="root.xr"\n', encoding="utf-8")
    uri, library = [(root / f"{name}.xr").as_uri() for name in ("root", "lib")]
    def invoke(*args):
        result = subprocess.run([str(exe.resolve()), uri, "--signature", library, *map(str, args)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=b"", env=env, timeout=290 if mode in ("--resources", "--generic-resources") else 20)
        assert result.returncode == 0, (args, result.returncode, result.stderr[-5000:])
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
        return result
    if mode == "--counterfactual":
        result = invoke(13)
        assert not result.stdout and b"SIGNATURE_BEFORE mode15_rejected=1 physical0/0" in result.stderr
        print("Fixed predecessor SourceTask rejects actual signature mode15")
        sys.exit(0)
    rows = []
    if mode == "--normal":
        for scenario in [*range(13), *range(14,25)]:
            result = invoke(scenario)
            if scenario in (9, 15):
                check_signature(json.loads(result.stdout))
                assert (b"SIGNATURE_SYNC actual_public_handler=1 Source_owner_dead=1" if scenario == 9 else b"SIGNATURE_DEATH server_poisoned=1 one_shot=1") in result.stderr
            elif scenario == 14:
                assert not result.stdout and b"synchronous_and_async=10" in result.stderr
            elif scenario in (4, 5, 6):
                assert not result.stdout
            else:
                messages = unpack(result.stdout)
                assert len(messages) == 1 and messages[0]["id"] == "source\0λ", messages
                if scenario in (1, 2, 8):
                    assert messages[0]["error"]["code"] == -32800, messages
                elif scenario == 3:
                    assert messages[0]["error"]["code"] == -32803, messages
                else:
                    assert "result" in messages[0], (scenario, messages)
                    check_signature(messages[0]["result"], scenario)
            rows.append({"scenario": scenario, "passed": True})
    measured = []
    variants = (int(sys.argv[4]),) if mode == "--generic-resources" else (0, 10, 11, 12, 18, 20, 21, 22, 24) if mode == "--normal" else (0, 10, 11, 12)
    assert all(v in (0, 10, 11, 12, 18, 20, 21, 22, 24) for v in variants)
    for variant in variants:
        result = invoke("--resource", maximum, maximum, variant)
        match = stats_re.search(result.stderr)
        assert match and all(int(match[n]) > 0 for n in (1, 2, 6, 7, 8)), result.stderr[-5000:]
        assert all(int(match[n]) == 0 for n in (3, 4, 5)), result.stderr[-5000:]
        messages = unpack(result.stdout)
        assert len(messages) == 1 and "error" not in messages[0], messages
        check_signature(messages[0]["result"], variant)
        measured.append(dict(variant=variant, source=int(match[1]), json=int(match[2]), allocated=int(match[6]), peak=int(match[7]), work=int(match[8])))
    if mode in ("--resources", "--generic-resources"):
        chunks = [(m["variant"], domain, start, min(64, m[domain]-start), m[domain]) for m in measured for domain in ("source", "json") for start in range(0, m[domain], 64)]
        def run_chunk(chunk):
            variant, domain, start, count, total = chunk
            result = invoke("--chunk", domain, start, count, total, variant)
            assert f"SIGNATURE_CHUNK domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0".encode() in result.stderr
            for value in unpack(result.stdout):
                assert value["error"]["code"] == -32603 and "result" not in value, value
            return dict(variant=variant, domain=domain, start=start, count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:
            rows.extend(f.result() for f in as_completed([pool.submit(run_chunk, c) for c in chunks]))
        for m in measured:
            result = invoke("--axes", m["allocated"], m["peak"], m["work"], m["variant"])
            assert b"SIGNATURE_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0" in result.stderr
            messages = unpack(result.stdout)
            assert sum("result" in value for value in messages) == 6, messages
            for value in messages:
                if "result" in value:
                    check_signature(value["result"], m["variant"])
                else:
                    assert value["error"]["code"] == -32603
    print(json.dumps(dict(mode=mode, image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), measured=measured,
        source_total=sum(m["source"] for m in measured), json_total=sum(m["json"] for m in measured), full_FI=mode in ("--resources", "--generic-resources"), rows=rows)))
