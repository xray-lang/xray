"""Compile real compiler output under strict MSVC C11 and reject GNU expressions."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--xray", type=Path, required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    compiler = args.xray.resolve()
    with tempfile.TemporaryDirectory(prefix="xray-generated-c11-") as temporary:
        scope = Path(temporary).resolve()
        source = scope / "multiply.xr"
        generated = scope / "multiply.c"
        output = scope / "multiply.obj"
        source.write_text("var a: i64 = 6\nvar b: i64 = 7\nprint(a*b)\n", encoding="utf-8")
        subprocess.run([str(compiler), "build", "--c-only", "-o", str(generated), str(source)],
                       cwd=root, check=True, timeout=60)
        text = generated.read_text(encoding="utf-8")
        code = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                      "", text, flags=re.S)
        if re.search(r"\(\s*\{", code):
            raise AssertionError("Generated C contains a GNU statement expression")
        subprocess.run([
            args.cc, "/nologo", "/std:c11", "/experimental:c11atomics", "/utf-8", "/W4", "/WX",
            "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
            "/I" + str(root / "src"), "/I" + str(root / "include"),
            "/c", str(generated), "/Fo" + str(output),
        ], cwd=root, check=True, timeout=60)
        if not output.is_file() or not output.stat().st_size:
            raise AssertionError("MSVC did not produce the generated translation unit")
    print("Real generated C passed strict MSVC C11 with no GNU statement expressions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
