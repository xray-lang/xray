from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import time

exe, stdlib = map(Path, sys.argv[1:3])
exe = exe.resolve()
flags = sys.argv[3:]
assert flags in ([], ["--normal"], ["--enum-required"], ["--probe"])
root = exe.parent / "rename-inputs #😀"
root.mkdir(exist_ok=True)
(root / "xray.toml").write_text(
    '[project]\nname="lsp-rename"\nversion="1.0.0"\nmain="root.xr"\n', encoding="utf-8"
)
entry, library, closed = root / "root.xr", root / "lib.xr", root / "closed.xr"
closed.write_text("export fn target(value:i64)->i64{return value}\n", encoding="utf-8")
closed_before = closed.read_bytes()
assert not entry.exists() and not library.exists()
environment = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve()))
command = [str(exe), entry.as_uri(), library.as_uri(), closed.as_uri()]
started = time.monotonic()
deadline = started + 290
image_before = hashlib.sha256(exe.read_bytes()).hexdigest()


def run_child(*arguments, structured=True):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("original 290-second rename execution deadline expired")
    result = subprocess.run(
        [*command, *map(str, arguments)], env=environment, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=min(290, remaining),
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if result.returncode:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise RuntimeError(f"rename child failed: {arguments}, exit={result.returncode}")
    if not structured:
        return result.stdout
    row = json.loads(result.stdout.splitlines()[-1])
    assert all(row[key] == 0 for key in ("physical_live", "physical_bytes", "json_live", "json_bytes"))
    return row


def all_resources(probe):
    serial = [json.loads(line) for line in run_child("--serial-baselines", structured=False).splitlines() if line.startswith("{")]
    original = [row for row in serial if row["phase"] == "baseline"]
    assert len(original) == 8 and [row["mode"] for row in original] == list(range(8))
    assert serial[-1]["phase"] == "serial" and serial[-1]["checked"] == 8 and serial[-1]["rejections"] == 9
    assert all(row[key] == 0 for row in serial for key in ("physical_live", "physical_bytes", "json_live", "json_bytes"))
    baselines = []
    for mode in range(8):
        row = run_child("--baseline", mode)
        assert row["phase"] == "baseline" and row["mode"] == mode
        assert 0 < row["allocated"] <= 64 * 1024 * 1024
        assert 0 < row["peak"] <= 8 * 1024 * 1024
        assert 0 < row["work"] <= 128000000
        assert row["source"] > 0 and row["json"] > 0
        assert all(row[key] == original[mode][key] for key in ("source", "json", "allocated", "peak", "work"))
        baselines.append(row)
    run_child("--enum-required", structured=False)
    workers = 1 if probe else 8
    axes = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        pending = {
            pool.submit(run_child, "--axes", row["mode"], row["allocated"], row["peak"], row["work"]): row["mode"]
            for row in baselines
        }
        for future in as_completed(pending):
            row = future.result()
            assert row["phase"] == "axes" and row["mode"] == pending[future] and row["checked"] == 9
            axes.append(row)
    assert len(axes) == 8 and sum(row["checked"] for row in axes) == 72
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
        pending = {pool.submit(run_child, "--sweep", *plan): plan for plan in plans}
        try:
            for future in as_completed(pending):
                row = future.result()
                domain, mode, begin, count, total = pending[future]
                assert row["phase"] == "sweep" and row["domain"] == domain and row["mode"] == mode
                assert row["start"] == begin and row["count"] == row["checked"] == count and row["total"] == total
                key = (mode, domain, begin)
                assert key not in seen
                seen[key] = (count, total)
        except BaseException:
            for future in pending:
                future.cancel()
            raise
    assert seen == expected
    checked = {
        domain: [sum(count for (mode, kind, _), (count, _) in seen.items() if mode == index and kind == domain) for index in range(8)]
        for domain in ("source", "json")
    }
    if not probe:
        assert all(checked[domain][row["mode"]] == row[domain] for row in baselines for domain in ("source", "json"))
    assert hashlib.sha256(exe.read_bytes()).hexdigest() == image_before
    assert time.monotonic() <= deadline
    print(json.dumps({
        "full": not probe, "qualification": "DEVELOPMENT_PROBE_ONLY" if probe else "COMPLETE_ORIGINAL_RENAME_CASES",
        "baselines": baselines, "checked": checked, "axes": 72, "normal_cases": 8, "rejections": 9,
        "original_enum_required": True, "workers": workers, "chunks": len(plans),
        "cold_matches_original_serial_baselines": True,
        "source_chunk_size": 64, "global_deadline_seconds": 290, "child_timeout_ceiling_seconds": 290,
        "image_sha256": image_before, "elapsed_seconds": time.monotonic() - started,
    }))


try:
    if flags in (["--normal"], ["--enum-required"]):
        print(run_child(*flags, structured=False), end="")
    else:
        all_resources(flags == ["--probe"])
finally:
    assert not entry.exists() and not library.exists()
    assert closed.read_bytes() == closed_before
