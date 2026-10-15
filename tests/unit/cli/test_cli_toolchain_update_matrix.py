"""Independent persisted-state oracles for every allocation-swept family."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import tomllib

exe, parent = map(lambda value: Path(value).resolve(), sys.argv[1:3])
parent.mkdir(parents=True, exist_ok=True)
resources = sys.argv[3:] == ["--resources"]
records = []
with tempfile.TemporaryDirectory(prefix="matrix-中-", dir=parent) as directory:
    root = Path(directory)
    home = root / "home"
    config = home / "Xray" / "toolchains.toml"
    config.parent.mkdir(parents=True)
    environment = dict(os.environ, LOCALAPPDATA=str(home))
    for phase in range(14):
        command = [str(exe), str(config), str(phase), "resources" if resources else "normal"]
        result = subprocess.run(command, env=environment, capture_output=True, timeout=300)
        records.append({"phase": phase, "argv": command, "exit": result.returncode,
                        "stdout": result.stdout.decode("utf-8", errors="replace"),
                        "stderr": result.stderr.decode("utf-8", errors="replace")})
        (parent / "records.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(records[-1]["stdout"], end="", flush=True)
        assert result.returncode == 0, records[-1]
        actual_path = Path(str(config) + ".nested") / "用户" / "deep" / "toolchains.toml" if phase == 4 else config
        if phase == 3:
            assert not config.exists()
            continue
        if phase == 6:
            assert config.read_bytes() == b"broken = [\n"
            continue
        actual = tomllib.loads(actual_path.read_text(encoding="utf-8"))
        assert actual["schema"] == 1
        assert actual["policy"]["allow_fallback"] == (phase == 4)
        assert actual["policy"].get("prefer_external_native", True) and actual["policy"].get("auto_probe", True)
        if phase == 2:
            assert "native" not in actual
        elif phase == 1:
            assert actual["native"]["compiler"] == "before.exe"
            assert actual["target"]["aarch64-windows-msvc"]["compiler"] == "C:/after/cl.exe"
        elif phase in (8, 9, 13):
            assert actual["native"] == {"provider": "msvc", "compiler": "before.exe"}
        elif phase == 7:
            assert actual["native"]["compiler"] == "a" * 4096
        elif phase == 10:
            assert actual["native"] == {"provider": "zig", "zig": "C:/owned/zig.exe"}
        elif phase == 11:
            assert actual["native"] == {"provider": "auto", "compiler": "C:/after/cl.exe", "zig": "C:/owned/zig.exe"}
        else:
            assert actual["native"] == {"provider": "msvc", "compiler": "C:/after/cl.exe"}
        if phase == 12:
            assert len(actual["target"]) == 16
            assert actual["target"]["aarch64-windows-msvc"]["compiler"] == "old-12.exe"
