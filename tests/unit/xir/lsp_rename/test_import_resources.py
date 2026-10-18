from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

exe, stdlib = (Path(value).resolve() for value in sys.argv[1:3])
assert sys.argv[3:] in ([], ["--probe"])
probe = bool(sys.argv[3:])
root = exe.parent / "import-resource-inputs #😀"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text(
    '[project]\nname="lsp-import-resources"\nversion="1.0.0"\nmain="root.xr"\n',
    encoding="utf-8",
)
entry, library = root / "root.xr", root / "lib.xr"
assert not entry.exists() and not library.exists()
command = [str(exe), entry.as_uri(), library.as_uri(), library.as_uri()]
environment = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib))
image = hashlib.sha256(exe.read_bytes()).hexdigest()
started = time.monotonic()
deadline = started + 290


def run(*arguments, serial=False):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("290-second import resource execution deadline expired")
    result = subprocess.run(
        [*command, *map(str, arguments)], env=environment,
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        timeout=min(290, remaining),
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if result.returncode:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise RuntimeError(f"import resource child failed: {arguments}, exit={result.returncode}")
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    assert rows and all(row[key] == 0 for row in rows for key in (
        "physical_live", "physical_bytes", "json_live", "json_bytes",
    ))
    if serial:
        return rows
    assert len(rows) == 1
    return rows[0]


try:
    original = run("--import-serial", serial=True)
    assert len(original) == 6 and [row["mode"] for row in original] == list(range(6))
    baselines = []
    for mode in range(6):
        row = run("--import-baseline", mode)
        assert row == original[mode] and row["phase"] == "baseline"
        assert 0 < row["allocated"] <= 64 * 1024 * 1024
        assert 0 < row["peak"] <= 8 * 1024 * 1024
        assert 0 < row["work"] <= 128000000 and row["source"] and row["json"]
        baselines.append(row)
    workers = 1 if probe else 8
    with ThreadPoolExecutor(max_workers=workers) as pool:
        axes = list(pool.map(lambda row: run(
            "--import-axes", row["mode"], row["allocated"], row["peak"], row["work"],
        ), baselines))
    assert all(row["phase"] == "axes" and row["mode"] == mode and row["checked"] == 9
               for mode, row in enumerate(axes))
    plans = []
    for row in baselines:
        for domain in ("source", "json"):
            total = row[domain]
            chunks = [(point, 1) for point in sorted({0, total // 2, total - 1})] if probe else [
                (begin, min(64, total - begin)) for begin in range(0, total, 64)
            ]
            plans.extend((domain, row["mode"], begin, count, total) for begin, count in chunks)
    expected = {(mode, domain, begin): (count, total) for domain, mode, begin, count, total in plans}
    assert len(expected) == len(plans)
    seen = {}
    with ThreadPoolExecutor(max_workers=workers) as pool:
        pending = {pool.submit(run, "--import-sweep", *plan): plan for plan in plans}
        for future in as_completed(pending):
            row = future.result()
            domain, mode, begin, count, total = pending[future]
            assert row["phase"] == "sweep" and row["domain"] == domain and row["mode"] == mode
            assert row["start"] == begin and row["count"] == row["checked"] == count and row["total"] == total
            key = (mode, domain, begin)
            assert key not in seen
            seen[key] = count, total
    assert seen == expected
    checked = {domain: [
        sum(count for (mode, kind, _), (count, _) in seen.items() if mode == index and kind == domain)
        for index in range(6)
    ] for domain in ("source", "json")}
    if not probe:
        assert all(checked[domain][row["mode"]] == row[domain] for row in baselines for domain in ("source", "json"))
    print(json.dumps(dict(
        full=not probe, baselines=baselines, checked=checked, axes=54, cases=6,
        workers=workers, chunks=len(plans), global_deadline_seconds=290,
        image_sha256=image, elapsed_seconds=time.monotonic() - started,
        qualification="DEVELOPMENT_PROBE_ONLY" if probe else "COMPLETE_SIX_IMPORT_CASES",
    )))
finally:
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == image
    assert not entry.exists() and not library.exists()
