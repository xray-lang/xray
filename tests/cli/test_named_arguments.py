"""Check declaration-owned named binding, evaluation order and portable output."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def command(arguments: list[str], expected: int = 0) -> subprocess.CompletedProcess:
    result = subprocess.run(arguments, capture_output=True, timeout=30, env={**os.environ, "NO_COLOR": "1"})
    assert result.returncode == expected, (arguments, result.returncode, result.stdout, result.stderr)
    return result


def binding_errors(binary: str, root: Path) -> None:
    head = "fn f(a:i64,b:i64=2)->i64{return a+b}\n"
    cases = [
        (head+'print(f(other:1))\n', "unknown named argument", "other:"),
        (head+'print(f(1,a:2))\n', "parameter is supplied more than once", "a:2"),
        (head+'print(f(a:1,a:2))\n', "parameter is supplied more than once", "a:2"),
        (head+'print(f(a:1,99))\n', "positional argument follows a named argument", "99"),
        (head+'print(f(b:2))\n', "required parameter is not supplied", "f(b:"),
        (head+'const alias=f\nprint(alias(a:1))\n', "named arguments require a direct declaration call", "a:1"),
        (head+'const alias=f\nprint(alias(1,b:2))\n', "named arguments require a direct declaration call", "b:2"),
        ('const alias=(a:i64)=>a\nprint(alias(a:1))\n', "named arguments require a direct declaration call", "a:1"),
        ('fn f(a:i64,...tail:i64)->i64{return a}\nprint(f(a:1,tail:2))\n', "rest arguments must be positional", "tail:2"),
        (head+'print(f(a:...(1,2)))\n', "a named argument supplies one value, not an argument spread", "a:..."),
        (head+'print(f(a:1,...()))\n', "positional argument follows a named argument", "...()"),
        (head+'print(f(1,2,99,a:3))\n', "too many call arguments", "99"),
        ('const xs=[1,2]\nprint(xs.reduce(other:0,callback:(a,b)=>a+b))\n', "unknown named argument", "other:"),
        ('const xs=[1,2]\nprint(xs.reduce(initial:0,initial:1,callback:(a,b)=>a+b))\n', "parameter is supplied more than once", "initial:1"),
        ('const xs=[1,2]\nprint(xs.reduce(initial:0,(a,b)=>a+b))\n', "positional argument follows a named argument", "(a,b)=>"),
        ('const xs=[1,2]\nprint(xs.reduce(initial:0))\n', "required parameter is not supplied", "reduce("),
        ('const xs=[1,2]\nprint(xs.map(callback:...(1,2)))\n', "a named argument supplies one value, not an argument spread", "callback:..."),
        ('var xs=[1,2]\nxs.fill(end:2)\n', "required parameter is not supplied", "fill("),
        ('var xs=[1,2]\nxs.fill(value:9,end:2,start:0,start:1)\n', "parameter is supplied more than once", "start:1"),
        ('print("abc".slice(stop:2,start:0))\n', "unknown named argument", "stop:"),
        ('var xs=[1,2]\nxs.set(value:9,0)\n', "positional argument follows a named argument", "0)"),
        ('const xs=[1,2]\nprint(xs.concat(arrays:[3]))\n', "rest arguments must be positional", "arrays:"),
        ('var xs={"a":1}\nxs.set(value:2,value:3,key:"a")\n', "parameter is supplied more than once", "value:3"),
        ('const xs={"a":1}\nprint(xs.get(other:"a"))\n', "unknown named argument", "other:"),
        ('const xs={"a":1}\nxs.forEach(visitor:(key,value)=>print(value))\n', "unknown named argument", "visitor:"),
        ('const xs=Set([1,2])\nxs.forEach(visitor:(value)=>print(value))\n', "unknown named argument", "visitor:"),
    ]
    for index, (source, message, marker) in enumerate(cases):
        path = root / f"binding-error-{index}.xr"
        path.write_text(source, encoding="utf-8")
        result = command([binary, "check", str(path)], 1)
        first = next(line for line in result.stderr.decode("utf-8").splitlines() if "error:" in line)
        at = source.rindex(marker)
        line = source[:at].count("\n") + 1
        column = at - source.rfind("\n", 0, at)
        assert first.endswith("error: E0327: " + message), (index, first)
        assert re.search(rf":{line}:{column}: error:", first), (index, line, column, first)


def generated_c(binary: str, root: Path) -> None:
    prefix = 'fn add(a:i64,b:i64=20)->i64{return a+b}\nfn mark(value:i64)->i64{print(value);return value}\n'
    temporaries = 'const second=mark(2)\nconst first=mark(1)\n'
    local_head, local_tail = 'fn invoke(){\n', '}\ninvoke()\n'
    pairs = [
        (prefix+'print(add(a:1,b:2))\n', prefix+'print(add(  1,  2))\n'),
        (prefix+'print(add(a:1))\n', prefix+'print(add(  1))\n'),
        (prefix+local_head+temporaries+'print(add(b:second,a:first))\n'+local_tail,
         prefix+local_head+temporaries+'print(add(  first,  second))\n'+local_tail),
        (prefix+local_head+'    const result=add(\n        b:mark(2),\n        a:mark(1)\n    )\n    print(result)\n'+local_tail,
         prefix+local_head+'\nconst b=  mark(2)\nconst a=  mark(1)\n    const result=add(a,b)\n    print(result)\n'+local_tail),
        ('fn invoke(){const xs=[1,2,3];print(xs.reduce(callback:(a,b)=>a+b,initial:4))}\ninvoke()\n',
         'fn invoke(){const xs=[1,2,3];print(xs.reduce(         (a,b)=>a+b,        4))}\ninvoke()\n'),
        ('fn invoke(){const xs=[1,2,3];const init=4;const cb=(a:i64,b:i64)=>a+b;print(xs.reduce(initial:init,callback:cb))}\ninvoke()\n',
         'fn invoke(){const xs=[1,2,3];const init=4;const cb=(a:i64,b:i64)=>a+b;print(xs.reduce(cb,init))}\ninvoke()\n'),
        ('fn invoke(){var xs=[1,2,3];xs.fill(value:9,start:0,end:2);print(xs[0])}\ninvoke()\n',
         'fn invoke(){var xs=[1,2,3];xs.fill(      9,      0,    2);print(xs[0])}\ninvoke()\n'),
        (prefix+local_head+'var xs=[1,2,3]\nconst v=mark(9)\nconst idx=mark(0)\nxs.set(value:v,index:idx)\nprint(xs[0])\n'+local_tail,
         prefix+local_head+'var xs=[1,2,3]\nconst v=mark(9)\nconst idx=mark(0)\nxs.set(idx,v)\nprint(xs[0])\n'+local_tail),
    ]
    source, output = root / "equivalent.xr", root / "equivalent.c"
    for index, pair in enumerate(pairs):
        products = []
        for text in pair:
            source.write_text(text, encoding="utf-8")
            command([binary, "build", "--c-only", str(source), "-o", str(output)])
            products.append(output.read_bytes())
        assert products[0] == products[1], (index, products[0], products[1])


def struct_spread_errors(binary: str, root: Path) -> None:
    target = 'struct Target { a:i64 }\n'
    source = 'struct Source { a:i64 }\n'
    cases = [
        (target+'struct Source { extra:i64 }\nconst value=Target{...Source{extra:1}}\n', 'unknown struct field'),
        (target+'struct Source { a:string }\nconst value=Target{...Source{a:"x"}}\n', 'struct spread field type does not match the target field'),
        (target+source+'const value=Target{...Source{a:1},...Source{a:2}}\n', 'struct field has more than one spread source; supply it explicitly'),
        ('struct Target { a:i64=0 }\n'+source+'const value=Target{...Source{a:1},...Source{a:2}}\n', 'struct field has more than one spread source; supply it explicitly'),
        (target+'const value=Target{...{"a":1}}\n', 'struct spread source must be a struct value'),
        (target+'const value=Target{...(1,2)}\n', 'struct spread source must be a struct value'),
        (target+'class Source { a:i64=1 }\nconst value=Target{...Source()}\n', 'struct spread source must be a struct value'),
        ('struct Target { a:i64; b:i64=0 }\n'+source+'const value=Target{...Source{a:1},a:2,a:3}\n', 'duplicate struct literal field'),
        (target+'struct Empty {}\nconst value=Target{...Empty{}}\n', 'omitted field requires an accessible declaration default'),
    ]
    for index, (text, message) in enumerate(cases):
        path = root / f"struct-spread-error-{index}.xr"
        path.write_text(text, encoding="utf-8")
        result = command([binary, "check", str(path)], 1)
        first = next(line for line in result.stderr.decode("utf-8").splitlines() if "error:" in line)
        assert first.endswith("error: " + message), (index, first)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    binary = str(parser.parse_args().binary.resolve(strict=True))
    fixtures = Path(__file__).resolve().parents[1] / "fixtures" / "named_arguments"
    command([binary, "check", str(fixtures / "declarations_check.xr")])
    command([binary, "check", str(fixtures / "refs_check.xr")])
    command([binary, "check", str(fixtures / "native_read_check.xr")])
    command([binary, "check", str(fixtures / "hash_methods_check.xr")])
    with tempfile.TemporaryDirectory(prefix="named-arguments-") as directory:
        root = Path(directory)
        binding_errors(binary, root)
        struct_spread_errors(binary, root)
        generated_c(binary, root)
        for name in ("values", "records", "struct_spread", "array_callbacks", "native_read", "hash_methods"):
            source = fixtures / (name + ".xr")
            expected = (fixtures / (name + ".out")).read_bytes()
            command([binary, "check", str(source)])
            vm = command([binary, "run", str(source)])
            assert vm.stdout.replace(b"\r\n", b"\n") == expected
            native = root / (name + (".exe" if os.name == "nt" else ""))
            command([binary, "build", str(source), "-o", str(native)])
            result = command([str(native)])
            assert result.stdout.replace(b"\r\n", b"\n") == expected
    print("Named calls: binding positions, defaults, generics, spread/rest, native optional holes, VM/native and eight byte-identical C pairs PASS")


if __name__ == "__main__":
    main()
