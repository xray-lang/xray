"""Verify independent VM/native outputs through real Unicode CLI paths."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def checked(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=180)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def expect(result):
    expected = "41\n中文\n".encode("utf-8")
    if result.stdout.replace(b"\r\n", b"\n") != expected or result.stderr:
        raise AssertionError((result.stdout, result.stderr))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", type=Path, required=True)
    compiler = str(parser.parse_args().xray.resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="xray-unicode-cli-") as temporary:
        scope = Path(temporary)
        root = scope / "工程 空格 😀"
        root.mkdir()
        (root / "模块.xr").write_text("export fn value()->i64 { return 41 }\n", encoding="utf-8")
        entry = root / "main.xr"
        entry.write_text('import { value } from "./模块"\nprint(value())\nprint("中文")\n', encoding="utf-8")
        expect(checked([compiler, "run", str(entry)], scope))
        expect(checked([compiler, "run", "main.xr"], root))
        for output in (scope / "native.exe", root / "产物 😀.exe"):
            checked([compiler, "build", "--native", str(entry), "-o", str(output)], scope)
            expect(checked([str(output)], scope))
    print("Unicode roots, relative cwd, imports, native publication: independent outputs PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
