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
stats_re = re.compile(rb"FOLDING source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) created=1 producer_dead=1 physical0/0 JSON0/0 app0/0")

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

def check_folding(value, variant=0):
    shift = 1 if variant == 11 else 0
    expected = [] if variant == 10 else [
        {"startLine": shift, "endLine": 5+shift, "kind": "region"},
        {"startLine": 2+shift, "endLine": 4+shift, "kind": "region"}]
    assert value == expected, (variant, value, expected)

with tempfile.TemporaryDirectory(prefix="folding-😀-#-", dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / "xray.toml").write_text('[project]\nname="folding"\nmain="root.xr"\n', encoding="utf-8")
    uri, library = [(root / f"{name}.xr").as_uri() for name in ("root", "lib")]
    def invoke(*args):
        result = subprocess.run([str(exe.resolve()), uri, "--folding", library, *map(str, args)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=b"", env=env, timeout=290 if mode == "--resources" else 20)
        assert result.returncode == 0, (args, result.returncode, result.stderr[-5000:])
        assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
        return result
    if mode == "--counterfactual":
        result = invoke(13)
        check_folding(json.loads(result.stdout))
        assert b"FOLDING_CACHE before=1 cache_replaced_AST_stale=1" in result.stderr
        print(result.stderr.decode("utf-8"))
        print("Exact predecessor replaces syntax owner while retaining the old AST pointer; all owners reclaimed")
        sys.exit(0)
    rows = []
    if mode == "--normal":
        for scenario in [*range(13), 14, 15]:
            result = invoke(scenario)
            if scenario in (12, 15):
                check_folding(json.loads(result.stdout))
                assert (b"cache_and_AST_preserved=1" if scenario==12 else b"server_poisoned=1 one_shot=1") in result.stderr
            elif scenario == 14:
                assert not result.stdout and b"capture_and_async=4" in result.stderr
            elif scenario in (4, 5, 6):
                assert not result.stdout
            else:
                messages = unpack(result.stdout)
                assert len(messages)==1 and messages[0]["id"]=="source\0λ",messages
                if scenario in (1,2,8): assert messages[0]["error"]["code"]==-32800,messages
                elif scenario==3: assert messages[0]["error"]["code"]==-32803,messages
                else:
                    assert "result" in messages[0],(scenario,messages)
                    check_folding(messages[0]["result"],scenario)
            if scenario==2:
                interior=re.search(rb"INTERIOR mode=17 trigger=4096 calls=(\d+)",result.stderr)
                assert interior and 4096<=int(interior[1])<=4160,result.stderr
            print(json.dumps(dict(case=scenario,stderr=result.stderr.decode("utf-8"),stdout=result.stdout.decode("utf-8")),ensure_ascii=False))
            rows.append({"scenario":scenario,"passed":True})
    measured = []
    for variant in (0, 9, 10, 11):
        result = invoke("--resource", maximum, maximum, variant)
        match = stats_re.search(result.stderr)
        assert match and all(int(match[n]) > 0 for n in (1, 2, 6, 7, 8)), result.stderr[-5000:]
        assert all(int(match[n]) == 0 for n in (3, 4, 5)), result.stderr[-5000:]
        messages = unpack(result.stdout)
        assert len(messages) == 1 and "error" not in messages[0], messages
        check_folding(messages[0]["result"], variant)
        measured.append(dict(variant=variant, source=int(match[1]), json=int(match[2]), allocated=int(match[6]), peak=int(match[7]), work=int(match[8])))
    if mode == "--normal":
        for source, js in ((0, maximum), (maximum, 0)):
            result=invoke("--resource",source,js,0)
            assert b"_hit=1" in result.stderr
            for value in unpack(result.stdout): assert value["error"]["code"]==-32603
    if mode == "--resources":
        chunks = [(m["variant"], domain, start, min(64, m[domain]-start), m[domain]) for m in measured for domain in ("source", "json") for start in range(0, m[domain], 64)]
        def run_chunk(chunk):
            variant, domain, start, count, total = chunk
            result = invoke("--chunk", domain, start, count, total, variant)
            assert f"FOLDING_CHUNK domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0".encode() in result.stderr
            for value in unpack(result.stdout):
                assert value["error"]["code"] == -32603 and "result" not in value, value
            return dict(variant=variant, domain=domain, start=start, count=count)
        with ThreadPoolExecutor(max_workers=8) as pool:
            rows.extend(f.result() for f in as_completed([pool.submit(run_chunk, c) for c in chunks]))
        for m in measured:
            result = invoke("--axes", m["allocated"], m["peak"], m["work"], m["variant"])
            assert b"FOLDING_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0" in result.stderr
            messages = unpack(result.stdout)
            assert sum("result" in value for value in messages) == 6, messages
            for value in messages:
                if "result" in value:
                    check_folding(value["result"], m["variant"])
                else:
                    assert value["error"]["code"] == -32603
    print(json.dumps(dict(mode=mode, image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), measured=measured,
        source_total=sum(m["source"] for m in measured), json_total=sum(m["json"] for m in measured), full_FI=mode=="--resources", rows=rows)))
