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
assert mode in ("--normal", "--resources", "--counterfactual")
env = {**os.environ, "XRAY_STDLIB_PATH": str(stdlib.resolve())}
maximum = str((1 << 64) - 1)
stats_re = re.compile(rb"PARAMETER source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) created=1 producer_dead=1 physical0/0 JSON0/0 app0/0")

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

def check_parameter(value, variant=0):
    parameters = [] if variant in (9, 10, 17, 18, 19) else [
        {"position": {"line": 3, "character": 8}, "label": "value:", "kind": 2,
         "paddingRight": True, "tooltip": "parameter mode: ref"},
        {"position": {"line": 3, "character": 17}, "label": "extra:", "kind": 2, "paddingRight": True}]
    types = [{"position": {"line": 2, "character": 9}, "label": ": i64", "kind": 1, "paddingLeft": True}] if variant in (16, 17, 20) else []
    later = [{"position": {"line": 3, "character": 31}, "label": ": i64", "kind": 1, "paddingLeft": True}] if types else []
    expected = types + parameters + later
    assert value == expected, (variant, value, expected)

with tempfile.TemporaryDirectory(prefix="parameter-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="parameter"\nmain="root.xr"\n', encoding="utf-8")
    uri, library = [(root / f"{name}.xr").as_uri() for name in ("root", "lib")]
    def invoke(*args):
        result = subprocess.run([str(exe.resolve()), uri, "--parameter", library, *map(str, args)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=b"", env=env, timeout=290 if mode == "--resources" else 20)
        assert result.returncode == 0, (args, result.returncode, result.stderr[-5000:])
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
        return result
    if mode == "--counterfactual":
        result = invoke(13)
        assert not result.stdout and b"PARAMETER_BEFORE mode16_rejected=1 physical0/0" in result.stderr
        print("Fixed predecessor SourceTask rejects prepared parameter mode16")
        sys.exit(0)
    rows = []
    if mode == "--normal":
        for scenario in [*range(13), *range(14, 21)]:
            result = invoke(scenario)
            if scenario in (15, 20):
                check_parameter(json.loads(result.stdout), scenario)
                assert b"PARAMETER_DEATH server_poisoned=1 one_shot=1" in result.stderr
            elif scenario == 14:
                assert not result.stdout and b"capture_and_async=18" in result.stderr
            elif scenario in (4, 5, 6):
                assert not result.stdout
            else:
                messages = unpack(result.stdout)
                assert len(messages) == 1 and messages[0]["id"] == "source\0λ", messages
                if scenario in (1, 2, 8):
                    assert messages[0]["error"]["code"] == -32800, messages
                elif scenario == 3:
                    assert messages[0]["error"]["code"] == -32803, messages
                elif scenario in (11, 12):
                    assert messages[0]["error"]["code"] == -32603, messages
                else:
                    assert "result" in messages[0], (scenario, messages)
                    check_parameter(messages[0]["result"], scenario)
            rows.append({"scenario": scenario, "passed": True})
    measured = []
    for variant in (0, 9, 10, 16, 17):
        result = invoke("--resource", maximum, maximum, variant)
        match = stats_re.search(result.stderr)
        assert match and all(int(match[n]) > 0 for n in (1, 2, 6, 7, 8)), result.stderr[-5000:]
        assert all(int(match[n]) == 0 for n in (3, 4, 5)), result.stderr[-5000:]
        messages = unpack(result.stdout)
        assert len(messages) == 1 and "error" not in messages[0], messages
        check_parameter(messages[0]["result"], variant)
        measured.append(dict(variant=variant, source=int(match[1]), json=int(match[2]), allocated=int(match[6]), peak=int(match[7]), work=int(match[8])))
    if mode == "--resources":
        chunks = [(m["variant"], domain, start, min(64, m[domain]-start), m[domain]) for m in measured for domain in ("source", "json") for start in range(0, m[domain], 64)]
        def run_chunk(chunk):
            variant, domain, start, count, total = chunk
            result = invoke("--chunk", domain, start, count, total, variant)
            assert f"PARAMETER_CHUNK domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0".encode() in result.stderr
            for value in unpack(result.stdout):
                assert value["error"]["code"] == -32603 and "result" not in value, value
            return dict(variant=variant, domain=domain, start=start, count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:
            rows.extend(f.result() for f in as_completed([pool.submit(run_chunk, c) for c in chunks]))
        for m in measured:
            result = invoke("--axes", m["allocated"], m["peak"], m["work"], m["variant"])
            assert b"PARAMETER_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0" in result.stderr
            messages = unpack(result.stdout)
            assert sum("result" in value for value in messages) == 6, messages
            for value in messages:
                if "result" in value:
                    check_parameter(value["result"], m["variant"])
                else:
                    assert value["error"]["code"] == -32603
    print(json.dumps(dict(mode=mode, image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), measured=measured,
        source_total=sum(m["source"] for m in measured), json_total=sum(m["json"] for m in measured), full_FI=mode=="--resources", rows=rows)))
