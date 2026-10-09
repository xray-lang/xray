"""Private subprocess environments exercise the public selection order."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="xray-source-paths-") as temporary:
    base = Path(temporary)
    cwd = base / "工作 cwd"
    cwd.mkdir()
    entry = cwd / "入口.xr"
    entry.write_text("print(42)\n", encoding="utf-8")
    outside = base / "标准 library"
    outside.mkdir()
    value = "中文 😀 value"
    count = 0
    def run(layout, mode, override, expected, origin=0, status=0, env_value=value, source="入口.xr"):
        global count
        count += 1
        home = base / f"layout{count}"
        exe_dir = home / "bin"
        exe_dir.mkdir(parents=True)
        executable = exe_dir / binary.name
        shutil.copyfile(binary, executable)
        if layout == "development":
            expected = home / "stdlib"
            expected.mkdir()
        elif layout == "installed":
            expected = home / "lib" / "xray" / "stdlib"
            expected.mkdir(parents=True)
        elif layout == "blocked":
            (home / "stdlib").write_text("not a directory", encoding="utf-8")
        env = os.environ.copy()
        env.pop("XRAY_STDLIB_PATH", None)
        env.pop("XRAY_TEST_VALUE", None)
        if override is not None:
            env["XRAY_STDLIB_PATH"] = str(override)
        if env_value is not None:
            env["XRAY_TEST_VALUE"] = env_value
        env["测试 key"] = "fixed 中文"
        env["XRAY_TEST_FORMULA"] = "abc"
        # Fixed independent expectation: source-file rejection cannot block the
        # entry-independent real stdlib selector. The last two cases have a
        # valid explicit stdlib override and deliberately invalid source files.
        selected_expected = override if source in ("missing.xr", ".") else expected
        selected_status = 0 if source in ("missing.xr", ".") else status
        command = [str(executable), mode, source, str(expected or ""), str(origin), str(status), env_value or "", str(executable), str(selected_expected or ""), str(selected_status)]
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, timeout=100)
        print(f"case={count} layout={layout} mode={mode} status={status}")
        print(result.stdout.decode("utf-8", errors="strict"), end="")
        if result.returncode:
            raise AssertionError(f"{command!r}\n{result.stderr.decode('utf-8', errors='replace')}")
    run("none", "matrix", outside, outside)
    run("development", "matrix", None, None, 1)
    run("installed", "normal", "", None, 2, env_value="")
    (cwd / "stdlib").mkdir()
    run("none", "absent", None, cwd / "stdlib", 3, env_value=None)
    run("none", "normal", base / "missing", None, status=2)
    run("none", "normal", entry, None, status=3)
    run("blocked", "normal", None, None, status=3)
    run("none", "normal", outside, None, status=2, source="missing.xr")
    run("none", "normal", outside, None, status=3, source=".")
    print(f"{count} independent real path/environment cases PASS")
