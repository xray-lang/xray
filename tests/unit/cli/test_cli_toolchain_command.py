"""Run real command owners inside an isolated platform preference namespace."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

exe, parent = (Path(x).resolve() for x in sys.argv[1:3])
parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="command-中-", dir=parent) as directory:
    root = Path(directory)
    env = dict(os.environ, LOCALAPPDATA=str(root / "用户配置"))
    config = Path(env["LOCALAPPDATA"]) / "Xray/toolchains.toml"
    config.parent.mkdir(parents=True)
    command = [str(exe), str(config), *sys.argv[3:]]
    result = subprocess.run(command, env=env, capture_output=True, timeout=590)
    record = {"argv": command, "exit": result.returncode, "stdout": result.stdout.decode("utf-8"),
              "stderr": result.stderr.decode("utf-8")}
    (parent / "record.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(record["stderr"], end="")
    assert result.returncode == 0, record
