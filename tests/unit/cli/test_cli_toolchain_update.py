"""Actual locked updates, independent TOML expectations and concurrent writers."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import tomllib

exe, parent = map(lambda value: Path(value).resolve(), sys.argv[1:3])
parent.mkdir(parents=True, exist_ok=True)
resources = sys.argv[3:] == ["--resources"]
records = []
with tempfile.TemporaryDirectory(prefix="transaction-中-", dir=parent) as directory:
    root = Path(directory)
    config = root / "用户设置" / "nested" / "toolchains.toml"

    def run(name, operation="set", target="native", compiler="C:/after/编译器/cl.exe", expected=0, path=config):
        command = [str(exe), str(path), operation, target, compiler, str(expected)]
        result = subprocess.run(command, capture_output=True, timeout=30)
        records.append({"case": name, "argv": command, "exit": result.returncode,
                        "stdout": result.stdout.decode("utf-8", errors="replace"),
                        "stderr": result.stderr.decode("utf-8", errors="replace")})
        assert result.returncode == 0, records[-1]
        print(name, result.stdout.decode("utf-8").strip(), flush=True)

    run("create-nested-unicode")
    assert tomllib.loads(config.read_text(encoding="utf-8"))["native"] == {
        "provider": "msvc", "compiler": "C:/after/编译器/cl.exe"}
    run("preserve-native", target="aarch64-linux-gnu", compiler="C:/other/cl.exe")
    data = tomllib.loads(config.read_text(encoding="utf-8"))
    assert data["native"]["compiler"] == "C:/after/编译器/cl.exe"
    assert data["target"]["aarch64-linux-gnu"]["compiler"] == "C:/other/cl.exe"
    escaped = 'C:/引用/quote"/tab\t\\cl.exe'
    run("escape-roundtrip", compiler=escaped)
    assert tomllib.loads(config.read_text(encoding="utf-8"))["native"]["compiler"] == escaped
    run("targeted-reset", operation="reset", compiler="-")
    data = tomllib.loads(config.read_text(encoding="utf-8"))
    assert "native" not in data and "aarch64-linux-gnu" in data["target"]
    assert data["policy"] == {"allow_fallback": True, "prefer_external_native": True, "auto_probe": True}
    original = config.read_bytes()
    run("invalid-target-preserves", target="not-a-real-target", expected=1)
    assert config.read_bytes() == original
    config.write_text("broken = [\n", encoding="utf-8")
    run("corrupt-preserved", expected=1)
    assert config.read_text(encoding="utf-8") == "broken = [\n"
    run("reset-all-repairs-corrupt", operation="reset-all", compiler="-")
    assert not config.exists()
    run("reset-missing-no-publish", operation="reset", compiler="-")
    assert not config.exists()
    directory_leaf = root / "directory-leaf"
    directory_leaf.mkdir()
    run("directory-leaf-refused", path=directory_leaf, expected=1)
    assert not (root / "directory-leaf.lock").exists()

    targets = ["aarch64-apple-darwin", "x86_64-apple-darwin", "aarch64-linux-gnu", "x86_64-linux-gnu",
               "x86_64-linux-musl", "aarch64-linux-musl", "x86_64-windows-msvc", "aarch64-windows-msvc"]
    children = [(target, subprocess.Popen([str(exe), str(config), "set", target, f"C:/owned/{i}/cl.exe", "0"],
                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)) for i, target in enumerate(targets)]
    for target, child in children:
        stdout, stderr = child.communicate(timeout=30)
        assert child.returncode == 0, (target, stdout, stderr)
    data = tomllib.loads(config.read_text(encoding="utf-8"))
    assert set(data["target"]) == set(targets), data
    for i, target in enumerate(targets):
        assert data["target"][target] == {"provider": "msvc", "compiler": f"C:/owned/{i}/cl.exe"}
    print("8 concurrent writers: all disjoint updates retained", flush=True)
    if resources:
        result = subprocess.run([str(exe), str(config), "faults"], capture_output=True, timeout=300)
        sys.stdout.buffer.write(result.stdout)
        sys.stdout.flush()
        assert result.returncode == 0, result.stderr.decode("utf-8", errors="replace")
    assert set(config.parent.iterdir()) == {config, Path(str(config) + ".lock")}
(parent / "records.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
