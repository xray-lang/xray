"""Check declaration-owned labels across captures with real VM/native products."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def command(args: list[str], status: int = 0) -> subprocess.CompletedProcess:
    result = subprocess.run(args, capture_output=True, timeout=30, env={**os.environ, "NO_COLOR": "1"})
    assert result.returncode == status, (args, result.returncode, result.stdout, result.stderr)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    binary = str(parser.parse_args().binary.resolve(strict=True))
    positives = [
        ("fn outer()->i64{fn add(a:i64,b:i64)->i64{return a*10+b};return add(b:2,a:3)};print(outer())", b"32\n"),
        ("fn mark(v:i64)->i64{print(v);return v}\nfn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};return add(b:mark(2),a:mark(1))};print(outer())", b"2\n1\n19\n"),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};fn relay()->i64{return add(b:2,a:3)};return relay()};print(outer())", b"39\n"),
        ("fn outer()->i64{const base=7;fn add(n:i64)->i64{if(n==0){return base};return add(n:n-1)};return add(n:3)};print(outer())", b"7\n"),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};fn relay()->i64{fn leaf()->i64{return add(b:2,a:3)};return leaf()};return relay()};print(outer())", b"39\n"),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64=2)->i64{return base+a+b};return add(4)};print(outer())", b"13\n"),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64=2)->i64{return base+a*10+b};fn relay()->i64{return add(a:3)};return relay()};print(outer())", b"39\n"),
        ("fn mark(v:i64)->i64{print(v);return v}\nfn outer()->i64{const base=7;fn add(a:i64,b:i64=mark(2),c:i64=3)->i64{return base+a*100+b*10+c};return add(c:mark(8),a:mark(1))};print(outer())", b"8\n1\n2\n135\n"),
        ("fn outer()->i64{const base=7;fn add(n:i64,step:i64=1)->i64{if(n==0){return base};return add(n:n-step)};return add(n:3)};print(outer())", b"7\n"),
        ("fn outer()->i64{const base=7;fn add(a:i64,...tail:i64)->i64{return base+a+len(tail)};return add(a:3)};print(outer())", b"10\n"),
    ]
    denied = "named arguments require a direct declaration call"
    negatives = [
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};const alias=add;return alias(a:1,b:2)};print(outer())", denied),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};fn relay()->i64{const alias=add;return alias(a:1,b:2)};return relay()};print(outer())", denied),
        ("fn apply(callback:fn(i64)->i64)->i64{return callback(a:1)};print(apply((a:i64)=>a))", denied),
        ("fn outer()->i64{const base=7;const add=(a:i64,b:i64)=>base+a+b;return add(a:1,b:2)};print(outer())", denied),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};return add(other:1,b:2)};print(outer())", "unknown named argument"),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};return add(a:1,a:2)};print(outer())", "parameter is supplied more than once"),
        ("fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a+b};return add(b:2,1)};print(outer())", "positional argument follows a named argument"),
    ]
    with tempfile.TemporaryDirectory(prefix="declared-closure-names-") as directory:
        root = Path(directory)
        source = root / "program.xr"
        native = root / ("program.exe" if os.name == "nt" else "program")
        output = root / "program.c"
        for index, (text, expected) in enumerate(positives):
            source.write_text(text + "\n", encoding="utf-8")
            command([binary, "check", str(source)])
            if index in (2, 4, 6, 9):
                # Passing a closure as another closure's environment is outside
                # the current LIR carrier. The same positional inputs reject.
                for action in ([binary, "run", str(source)], [binary, "build", str(source), "-o", str(native)]):
                    result = command(action, 1)
                    boundary = "throwing or variadic callable ABI" if index == 9 else "FUNCTION_REF captured physical binding transport"
                    assert result.stderr.decode("utf-8").strip().endswith(
                        boundary + " is not supported by LIR lowering yet")
                continue
            assert command([binary, "run", str(source)]).stdout.replace(b"\r\n", b"\n") == expected
            command([binary, "build", str(source), "-o", str(native)])
            assert command([str(native)]).stdout.replace(b"\r\n", b"\n") == expected
        for text in (
            "fn outer()->i64{const base=7;fn add(a:i64,b:i64)->i64{return base+a*10+b};const relay=()=>add(b:2,a:3);return relay()};print(outer())",
            "fn outer(){const base=7;fn values(n:i64=2)->Iterator<i64>{yield base+n};const values_now=values()};outer()",
            "fn outer<T>(value:T)->T{fn choose(a:T,b:i64=2)->T{return a};return choose(a:value)};print(outer(3))",
        ):
            source.write_text(text + "\n", encoding="utf-8")
            command([binary, "check", str(source)])
        for text, diagnostic in negatives:
            source.write_text(text + "\n", encoding="utf-8")
            result = command([binary, "check", str(source)], 1)
            first = next(line for line in result.stderr.decode("utf-8").splitlines() if "error:" in line)
            assert first.endswith("error: E0327: " + diagnostic), (first, diagnostic)
        head = "fn outer()->i64{const base=7;fn add(a:i64,b:i64DEFAULT)->i64{return base+a*10+b};"
        pairs = [
            (head.replace("DEFAULT", "") + "return add(a:3,b:2)};print(outer())\n",
             head.replace("DEFAULT", "") + "return add(  3,  2)};print(outer())\n"),
            (head.replace("DEFAULT", "=2") + "return add(a:3)};print(outer())\n",
             head.replace("DEFAULT", "=2") + "return add(  3)};print(outer())\n"),
            (head.replace("DEFAULT", "") + "const first=3;const second=2;return add(b:second,a:first)};print(outer())\n",
             head.replace("DEFAULT", "") + "const first=3;const second=2;return add(  first,  second)};print(outer())\n"),
        ]
        for pair in pairs:
            products = []
            for text in pair:
                source.write_text(text, encoding="utf-8")
                command([binary, "build", "--c-only", str(source), "-o", str(output)])
                products.append(output.read_bytes())
            assert products[0] == products[1], "named closure arguments changed generated C"
    print("Declared closure names: thirteen checked declarations, six VM/native products, four existing LIR boundaries, seven exact diagnostics and three byte-identical C pairs PASS")


if __name__ == "__main__":
    main()
