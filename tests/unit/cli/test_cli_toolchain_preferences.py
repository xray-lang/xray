"""Exercise public preference commands against real private persistent state."""
from pathlib import Path
import concurrent.futures
import json
import os
import subprocess
import sys
import tempfile

exe, parent = (Path(value).resolve() for value in sys.argv[1:3])
parent.mkdir(parents=True, exist_ok=True)
records = []
with tempfile.TemporaryDirectory(prefix="preferences-中-", dir=parent) as directory:
    root = Path(directory)
    environment = dict(os.environ, LOCALAPPDATA=str(root / "用户配置"))
    config = Path(environment["LOCALAPPDATA"]) / "Xray" / "toolchains.toml"

    def run(name, arguments, expected=0, machine=True):
        command = [str(exe), "toolchain", *arguments]
        if machine:
            command.append("--json")
        result = subprocess.run(command, env=environment, capture_output=True, timeout=30)
        record = dict(case=name, argv=command, exit=result.returncode,
                      stdout=result.stdout.decode("utf-8"), stderr=result.stderr.decode("utf-8"))
        records.append(record)
        (parent / "records.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        assert result.returncode == expected, record
        if expected:
            assert not result.stdout, record
            return None
        assert not result.stderr, record
        if not machine:
            return record["stdout"]
        value = json.loads(result.stdout)
        assert value["schema"] == 1, record
        return value

    path = run("absent-config-path", ["config-path"])["path"]
    assert Path(path) == config and not config.parent.exists()
    state = run("absent-list", ["list"])
    assert not state["exists"] and state["preferences"] == [] and not config.parent.exists()
    compiler = str(root / '编译器 "quoted"' / "cl.exe")
    selected = run("native-use", ["use", "--target", "native", "msvc", "--cc", compiler])
    assert selected["updated"] and selected["provider"] == "msvc" and Path(selected["configPath"]) == config
    assert config.is_file()
    state = run("native-list", ["list"])
    assert state["exists"] and state["preferences"] == [dict(target="native", provider="msvc", compiler=compiler, zig=None)]
    run("other-use", ["use", "--target", "aarch64-windows-msvc", "host"])
    state = run("two-targets-list", ["list"])
    assert {row["target"] for row in state["preferences"]} == {"native", "aarch64-windows-msvc"}
    original = config.read_bytes()
    for name, arguments in (
        ("unknown-provider", ["use", "--target", "native", "invented"]),
        ("missing-target", ["use", "msvc"]),
        ("provider-path-mismatch", ["use", "--target", "native", "zig", "--cc", compiler]),
        ("reset-path-rejected", ["reset", "--cc", compiler]),
        ("list-target-rejected", ["list", "--target", "native"]),
        ("config-path-extra-rejected", ["config-path", "extra"]),
    ):
        run(name, arguments, expected=2)
        assert config.read_bytes() == original
    run("target-reset", ["reset", "--target", "native"])
    state = run("target-reset-list", ["list"])
    assert [row["target"] for row in state["preferences"]] == ["aarch64-windows-msvc"]
    run("reset-all", ["reset"])
    assert not config.exists()
    assert run("plain-config-path", ["config-path"], machine=False).strip() == str(config)
    malformed = b"schema = 1\n[native]\nprovider = 42\n"
    config.write_bytes(malformed)
    run("corrupt-list-rejected", ["list"], expected=1)
    run("corrupt-use-preserved", ["use", "--target", "native", "msvc"], expected=1)
    run("corrupt-target-reset-preserved", ["reset", "--target", "native"], expected=1)
    assert config.read_bytes() == malformed
    assert Path(run("corrupt-config-path", ["config-path"])["path"]) == config
    run("corrupt-reset-all", ["reset"])
    assert not config.exists()
    policy = 'schema = 1\n[policy]\nallow_fallback = false\nprefer_external_native = false\nauto_probe = false\n'
    config.write_text(policy, encoding="utf-8", newline="\n")
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        def update(target):
            result = subprocess.run([str(exe), "toolchain", "use", "--target", target, "msvc", "--json"],
                                    env=environment, capture_output=True, timeout=30)
            assert result.returncode == 0 and not result.stderr, (target, result)
            assert json.loads(result.stdout)["updated"]
            return target
        assert set(pool.map(update, ("native", "aarch64-windows-msvc"))) == {"native", "aarch64-windows-msvc"}
    state = run("concurrent-updates-retained", ["list"])
    assert {row["target"] for row in state["preferences"]} == {"native", "aarch64-windows-msvc"}
    assert not state["allowFallback"] and not state["preferExternalNative"] and not state["autoProbe"]
    run("final-reset", ["reset"])
    assert not config.exists()
print(json.dumps(dict(status="PASS", commands=len(records) + 2, concurrent_writers=2,
                      corrupt_preserved=True, unicode=True, original_owned_transaction=True)))
