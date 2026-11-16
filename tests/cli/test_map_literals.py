"""Observe strict Map literals through real checking, bytecode and native products."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def command(arguments: list[str], success: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(arguments, capture_output=True, timeout=30, env={**os.environ, "NO_COLOR": "1"})
    assert (result.returncode == 0) == success, (arguments, result.returncode, result.stdout, result.stderr)
    return result


def position(source: str, offset: int) -> tuple[int, int]:
    return source[:offset].count("\n") + 1, len(source[source.rfind("\n", 0, offset) + 1:offset].encode("utf-8")) + 1


def static_keys(binary: str, root: Path) -> None:
    cases = [
        ('', '"same"', '"same"', '"same"'),
        ('', '"名字"', '"名字"', '"名字"'),
        ('', '"' + 'x' * 4096 + '"', '"' + 'x' * 4096 + '"', '"' + 'x' * 4096 + '"'),
        ('', '"a\\0b"', '"a\\0b"', '"a\\u0000b"'),
        ('', '-1', '-1', '-1'),
        ('', 'true', 'true', 'true'),
        ('', "'a'", "'a'", 'U+0061'),
        ('enum Choice { A, B }\n', 'Choice.A', 'Choice.A', 'Choice.A'),
        ('const ORIGINAL="same"\nconst ALIAS=ORIGINAL\n', 'ORIGINAL', 'ALIAS', '"same"'),
        ('const FIRST=comptime (1+1)\n', 'FIRST', '2', '2'),
        ('', 'comptime ("same")', '"same"', '"same"'),
    ]
    path = root / 'static.xr'
    for prefix, first, second, spelling in cases:
        literal = '{' + first + ':1, ' + second + ':2}'
        source = prefix + 'const value=' + literal + '\n'
        path.write_text(source, encoding='utf-8')
        result = command([binary, 'check', str(path)], False)
        lines = result.stderr.decode('utf-8').splitlines()
        errors = [line for line in lines if 'error:' in line]
        notes = [line for line in lines if 'note:' in line]
        assert len(errors) == 1 and errors[0].endswith(f'error: E0325: duplicate key {spelling} in Map literal'), lines
        assert len(notes) == 1 and notes[0].endswith('note: the key first appears here'), lines
        start = len(prefix) + len('const value={')
        a, b = position(source, start), position(source, start + len(first) + len(':1, '))
        assert f':{b[0]}:{b[1]}: error:' in errors[0] and f':{a[0]}:{a[1]}: note:' in notes[0], lines
    source = 'const value="${({"same":1,"same":2})}"\n'
    path.write_text(source, encoding='utf-8')
    lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
    first, second = source.index('"same"'), source.rindex('"same"')
    a, b = position(source, first), position(source, second)
    assert any(f':{b[0]}:{b[1]}: error: E0325:' in line for line in lines), lines
    assert any(f':{a[0]}:{a[1]}: note: the key first appears here' in line for line in lines), lines


def products(binary: str, root: Path) -> None:
    source = '''fn key(value:string)->string {print(value);return value}
fn value(label:string,result:i64)->i64 {print(label);return result}
fn defaults()->Map<string,i64> {print("spread");return {"a":2,"b":3}}
var result={key("a"):value("first",1),...defaults(),key("c"):value("last",4)}
print(len(result));print(result["a"]);print(result["b"]);print(result["c"])
var empty:Map<string,i64>={};print(len(empty))
print("${({"a":7})["a"]}")
const ORIGINAL="same"
fn shadow() {const alias=ORIGINAL;const ORIGINAL="other";print(len({alias:1,ORIGINAL:2}))}
shadow()
var nul={"a\\0b":11,"a\\0c":13};print(nul["a\\0b"]);print(nul["a\\0c"])
fn part(value:i64)->i64{print(value);return value}
fn numbers()->Array<i64>{print(88);return [5,6]}
var joined=[part(4),...numbers(),part(7)]
print(joined[0]);print(joined[1]);print(joined[2]);print(joined[3])
'''
    expected = b'a\nfirst\nspread\nc\nlast\n3\n2\n3\n4\n0\n7\n2\n11\n13\n4\n88\n7\n4\n5\n6\n7\n'
    path = root / 'products.xr'
    path.write_text(source, encoding='utf-8')
    command([binary, 'check', str(path)])
    vm = command([binary, 'run', str(path)])
    assert vm.stdout.replace(b'\r\n', b'\n') == expected, vm
    native = root / ('products.exe' if os.name == 'nt' else 'products')
    command([binary, 'build', str(path), '-o', str(native)])
    actual = command([str(native)])
    assert actual.stdout.replace(b'\r\n', b'\n') == expected, actual
    generated = root / 'products.c'
    command([binary, 'build', '--c-only', str(path), '-o', str(generated)])
    text = generated.read_text(encoding='utf-8')
    assert 'xrt_native_map_literal_create(' in text and 'xrt_map_string_i64_literal(' not in text
    assert not re.search(r'\(\s*\{', text)


def dynamic_keys(binary: str, root: Path) -> None:
    cases = [
        'fn key()->string{return "same"}\nconst value={key():1,key():2}\n',
        'var key="same"\nconst value={key:1,key:2}\n',
        'fn key()->string{return "same"}\nconst value={key():1,...{"same":8},key():2}\n',
        'fn key()->string{return "same"}\nconst value="${({key():1,key():2})["same"]}"\n',
    ]
    path = root / 'dynamic.xr'
    native = root / ('dynamic.exe' if os.name == 'nt' else 'dynamic')
    # Check the syntactic rule independently of the String/I64 native family.
    path.write_text('const key=1+1\nconst value={key:1,2:2}\n', encoding='utf-8')
    command([binary, 'check', str(path)])
    for source in cases:
        path.write_text(source, encoding='utf-8')
        command([binary, 'check', str(path)])
        vm = command([binary, 'run', str(path)], False)
        command([binary, 'build', str(path), '-o', str(native)])
        actual = command([str(native)], False)
        for result in (vm, actual):
            message = result.stderr.decode('utf-8')
            assert 'E0434: duplicate key "same" in Map literal (entries at ' in message and ' and ' in message, message
            assert not result.stdout, result


def collection_types(binary: str, root: Path) -> None:
    cases = [
        ('var user={"name":"A","age":30}\n', 'Map literal values have different types: string and i64',
         'the first value is string; without a declared Map type, every value must be string',
         'for a fixed set of fields, declare a struct: struct User { name: string; age: i64 }, then write User{name: "A", age: 30}'),
        ('var values=[1,"a"]\n', 'Array literal elements have different types: i64 and string',
         'the first element is i64; without a declared Array type, every element must be i64',
         'for a fixed number of elements, use a tuple: (1, "a")'),
        ('var values=[Set([1]),Set(["a"])]\n', 'Array literal elements have different types: Set<i64> and Set<string>',
         'the first element is Set<i64>; without a declared Array type, every element must be Set<i64>',
         'for a fixed number of elements, use a tuple: (Set([1]), Set(["a"]))'),
        ('var value={"a":1,2:3}\n', 'Map literal keys have different types: string and i64',
         'the first key is string; without a declared Map type, every key must be string',
         'declare Map<K, V> with one key type for all entries'),
        ('var values=[{"a":1},{"b":"x"}]\n',
         'Array literal elements have different types: Map<string, i64> and Map<string, string>',
         'the first element is Map<string, i64>; without a declared Array type, every element must be Map<string, i64>',
         'for a fixed number of elements, use a tuple: ({"a":1}, {"b":"x"})'),
    ]
    path = root / 'heterogeneous.xr'
    for source, message, first, repair in cases:
        path.write_text(source, encoding='utf-8')
        result = command([binary, 'check', str(path)], False)
        lines = result.stderr.decode('utf-8').splitlines()
        errors = [line for line in lines if 'error:' in line]
        notes = [line for line in lines if 'note:' in line]
        assert len(errors) == 1 and errors[0].endswith('error: E0326: ' + message), lines
        union = ('to keep both types in one Map, declare Map<string, string | i64>' if source.startswith('var user=')
                 else 'to keep both types in one Array, declare Array<i64 | string>' if source.startswith('var values=[1,') else None)
        json_hint = source.startswith('var user=')
        assert len(notes) == 2 + bool(union) + json_hint and notes[0].endswith('note: ' + first) and notes[1].endswith('note: ' + repair), lines
        if json_hint:
            assert notes[2].endswith('note: for JSON data, declare the type: var user: JSON.Object = {...}'), lines
            assert ':1:5: note:' in notes[2], lines
            path.write_text(source.replace('var user=', 'var user:JSON.Object='), encoding='utf-8')
            command([binary, 'check', str(path)])
        if union:
            assert notes[2 + json_hint].endswith('note: ' + union), lines
            binding = 'user' if source.startswith('var user=') else 'values'
            assert ':1:5: note:' in notes[2 + json_hint], lines
            annotation = 'Map<string, string | i64>' if binding == 'user' else 'Array<i64 | string>'
            path.write_text(source.replace('var '+binding+'=', 'var '+binding+':'+annotation+'='), encoding='utf-8')
            command([binary, 'check', str(path)])
    for key in ('"not valid"', '"fn"', '""', 'key'):
        source = 'const key="name"\nvar user={' + key + ':"A","age":30}\n'
        path.write_text(source, encoding='utf-8')
        lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
        assert sum('error: E0326:' in line for line in lines) == 1 and sum('note:' in line for line in lines) == 3, lines
        assert any(line.endswith('note: for JSON data, declare the type: var user: JSON.Object = {...}') for line in lines), lines
        assert lines[-1].endswith('note: to keep both types in one Map, declare Map<string, string | i64>'), lines
    for source, first, second in [
        ('var values=[...[1],"a"]\n', '...', '"a"'),
        ('var values=[1,...["a"]]\n', '1', '...'),
        ('var values=[...[1],...["a"]]\n', '...', '...'),
        ('const value="${[...[1],"a"]}"\n', '...', '"a"'),
    ]:
        path.write_text(source, encoding='utf-8')
        lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
        errors = [line for line in lines if 'error:' in line]
        notes = [line for line in lines if 'note:' in line]
        a, b = position(source, source.index(first)), position(source, source.rindex(second))
        assert len(errors) == 1 and errors[0].endswith('error: E0326: Array literal elements have different types: i64 and string'), lines
        assert len(notes) == 2 and notes[0].endswith('note: the first element is i64; without a declared Array type, every element must be i64'), lines
        assert notes[1].endswith('note: to keep both types in one Array, declare Array<i64 | string>'), lines
        assert f':{b[0]}:{b[1]}: error:' in errors[0] and f':{a[0]}:{a[1]}: note:' in notes[0], lines
    for source in (
        'struct Item { value:i64 }\nvar user={"name":"A","item":Item{value:1}}\n',
        'const n:i8=1\nvar user={"name":"A","n":n}\n',
        'var user={1:"A",2:30}\n',
        'class JSON {}\nvar user={"name":"A","age":30}\n',
        'type JSON=i64\nvar user={"name":"A","age":30}\n',
        'fn test<JSON>() { var user={"name":"A","age":30} }\n',
        'fn test() { const JSON=1; var user={"name":"A","age":30} }\n',
        'fn test() { type JSON=i64; var user={"name":"A","age":30} }\n',
        'fn test<T>() { type JSON=i64; var user={"name":"A","age":30} }\n',
        'fn test() {{ type JSON=i64 }; var user={"name":"A","age":30}}\n',
    ):
        path.write_text(source, encoding='utf-8')
        lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
        assert sum('error: E0326:' in line for line in lines) == 1, lines
        assert not any('for JSON data' in line for line in lines), lines
    for source in (
        'fn test() { var user={"name":"A","age":30}; type JSON=i64 }\n',
        'fn unrelated<T>() {type JSON=i64}\nvar user={"name":"A","age":30}\n',
    ):
        path.write_text(source, encoding='utf-8')
        lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
        assert sum('error: E0326:' in line for line in lines) == 1, lines
        assert any('for JSON data' in line for line in lines), lines
        path.write_text(source.replace('var user=', 'var user:JSON.Object='), encoding='utf-8')
        command([binary, 'check', str(path)])
    source = 'const last:JSON.Value=30\nvar user={"first":"A","last":last}\n'
    path.write_text(source, encoding='utf-8')
    lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
    assert any(line.endswith('note: for JSON data, declare the type: var user: JSON.Object = {...}') for line in lines), lines
    path.write_text(source.replace('var user=', 'var user:JSON.Object='), encoding='utf-8')
    command([binary, 'check', str(path)])
    path.write_text('const first:JSON.Value="A"\nvar user={"first":first,"age":30}\n', encoding='utf-8')
    command([binary, 'check', str(path)])
    source = 'fn f(value:string)->string{return value}\nvar user={"name":"A","age":f(1)}\n'
    path.write_text(source, encoding='utf-8')
    lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
    assert sum('error:' in line for line in lines) == 1 and not any('E0326:' in line for line in lines), lines
    source = 'fn f(value:string)->string{return value}\nvar values=[1,...[f(1)]]\n'
    path.write_text(source, encoding='utf-8')
    lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
    assert sum('error:' in line for line in lines) == 1 and not any('E0326:' in line for line in lines), lines
    path.write_text('struct User { name:string; age:i64 }\nvar user=User{name:"A",age:30}\nprint(user.name)\nprint(user.age)\n', encoding='utf-8')
    result = command([binary, 'run', str(path)])
    assert result.stdout.replace(b'\r\n', b'\n') == b'A\n30\n', result


def bare_keys(binary: str, root: Path) -> None:
    path = root / 'bare-keys.xr'
    for name in ('missing', 'key_' + 'q' * 4096):
        path.write_text(f'const value={{{name}:1}}\n', encoding='utf-8')
        result = command([binary, 'check', str(path)], False)
        lines = result.stderr.decode('utf-8').splitlines()
        assert sum('error:' in line for line in lines) == 1, lines
        assert sum('note:' in line for line in lines) == 1, lines
        assert any(line.endswith(f'note: if you intended a string key, write "{name}"') for line in lines), lines
    path.write_text('const key=1\nconst value:Map<string,i64>={key:1}\n', encoding='utf-8')
    result = command([binary, 'check', str(path)], False)
    assert b'if you intended a string key, write "key"' in result.stderr, result
    path.write_text('const key=1\nconst value={"a":1,key:2}\n', encoding='utf-8')
    result = command([binary, 'check', str(path)], False)
    assert b'E0326:' in result.stderr and b'if you intended a string key, write "key"' in result.stderr, result
    assert sum('note:' in line for line in result.stderr.decode('utf-8').splitlines()) == 3, result
    for source in ('const value={"name":missing}\n', 'const value={missing():1}\n'):
        path.write_text(source, encoding='utf-8')
        result = command([binary, 'check', str(path)], False)
        assert b'if you intended a string key' not in result.stderr, result
    path.write_text('const key="name"\nconst value={key:1}\n', encoding='utf-8')
    command([binary, 'check', str(path)])


def adoptable_repairs(binary: str, root: Path) -> None:
    path = root / 'repair-admission.xr'
    cases = [
        ('var first={"name":"A"}\nvar user={...first,"age":30}\n', 'user', None, False),
        ('var first=[1]\nvar values=[...first,"a"]\n', 'values', None, False),
        ('var first=[1]\nvar values=[...[...first],"a"]\n', 'values', None, False),
        ('var user={...{"name":"A"},"age":30}\n', 'user', 'Map<string, string | i64>', True),
        ('var values=[...[...[1]],"a"]\n', 'values', 'Array<i64 | string>', False),
        ('const first:Array<i64|string>=[1]\nvar values=["a",...first]\n', 'values', 'Array<string | i64>', False),
        ('const first:JSON.Object={"name":"A"}\nvar user={"age":30,...first}\n', 'user', None, True),
        ('const key=[1]\nvar value={key:1,key:"a"}\n', 'value', None, False),
    ]
    for source, binding, union, json_hint in cases:
        path.write_text(source, encoding='utf-8')
        lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
        assert sum('error: E0326:' in line for line in lines) == 1, lines
        assert any('to keep both types' in line for line in lines) == bool(union), lines
        assert any('for JSON data' in line for line in lines) == json_hint, lines
        for annotation in ([union] if union else []) + (['JSON.Object'] if json_hint else []):
            path.write_text(source.replace('var '+binding+'=', 'var '+binding+':'+annotation+'='), encoding='utf-8')
            command([binary, 'check', str(path)])


def constructor_visibility(binary: str, root: Path) -> None:
    path = root / 'constructor-scope.xr'
    (root / 'constructor-library.xr').write_text('export class Thing{}\n', encoding='utf-8')
    for name, statement, annotation in (
        ('Map', 'var user={"name":"A","age":30}', 'Map<string,string|i64>'),
        ('Array', 'var values=[1,"a"]', 'Array<i64|string>'),
    ):
        binding = 'user' if name == 'Map' else 'values'
        phrase = 'to keep both types in one ' + name
        alias = f'type {name}' + ('<K,V>' if name == 'Map' else '<T>') + ' = i64'
        shadowed = [
            f'class {name}{{}}\n{statement}\n',
            f'struct {name}{{}}\n{statement}\n',
            f'enum {name}{{A}}\n{statement}\n',
            f'fn {name}(){{}}\n{statement}\n',
            f'const {name}=1\n{statement}\n',
            f'var {name}=1\n{statement}\n',
            f'fn f<{name}>(){{{statement}}}\n',
            f'fn f(){{const {name}=1;{statement}}}\n',
            f'import "./constructor-library" as {name}\n{statement}\n',
            f'import {{Thing as {name}}} from "./constructor-library"\n{statement}\n',
            alias + '\n' + statement + '\n',
            f'fn f(){{{alias};{statement}}}\n',
            f'fn earlier(){{{alias}}}\n{statement}\n',
        ]
        for source in shadowed:
            path.write_text(source, encoding='utf-8')
            lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
            assert any('error: E0326:' in line for line in lines) and not any(phrase in line for line in lines), lines
            if name == 'Map':
                assert any('for JSON data' in line for line in lines), lines
                path.write_text(source.replace('var '+binding+'=', 'var '+binding+':JSON.Object='), encoding='utf-8')
                command([binary, 'check', str(path)])
        # Parser aliases obey written order; generic owners alone open temporary scopes.
        for source in (
            statement + '\n',
            f'fn f(){{{statement}}}\n',
            f'fn unrelated<T>(){{{alias}}}\n{statement}\n',
            f'{statement}\n{alias}\n',
            f'fn f(){{{statement};{alias}}}\n',
        ):
            path.write_text(source, encoding='utf-8')
            lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
            assert any('error: E0326:' in line for line in lines) and any(phrase in line for line in lines), lines
            path.write_text(source.replace('var '+binding+'=', 'var '+binding+':'+annotation+'='), encoding='utf-8')
            command([binary, 'check', str(path)])


def nominal_key_repairs(binary: str, root: Path) -> None:
    (root / 'key-library.xr').write_text(
        'export struct Original implements Hashable{value:i64;'
        'equals(other:Original)->bool{return this.value==other.value};'
        'hash()->i64{return this.value}}\n', encoding='utf-8')
    path = root / 'key-spelling.xr'
    for prefix, constructor, annotation, shown in (
        ('import {Original} from "./key-library"\n', 'Original', 'Original', True),
        ('import {Original as Key} from "./key-library"\n', 'Key', 'Key', False),
        ('import "./key-library" as keys\n', 'keys.Original', 'keys.Original', False),
        ('import {Original as Key} from "./key-library"\nclass Original{}\n', 'Key', 'Key', False),
    ):
        source = prefix + 'const key=' + constructor + '{value:1}\nvar value={key:1,key:"a"}\n'
        path.write_text(source, encoding='utf-8')
        lines = command([binary, 'check', str(path)], False).stderr.decode('utf-8').splitlines()
        assert sum('error: E0326:' in line for line in lines) == 1, lines
        assert any('to keep both types' in line for line in lines) == shown, lines
        path.write_text(source.replace('var value=', 'var value:Map<'+annotation+',i64|string>='), encoding='utf-8')
        command([binary, 'check', str(path)])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    binary = str(parser.parse_args().binary.resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix='map-literals-') as directory:
        root = Path(directory)
        static_keys(binary, root)
        products(binary, root)
        dynamic_keys(binary, root)
        collection_types(binary, root)
        bare_keys(binary, root)
        adoptable_repairs(binary, root)
        constructor_visibility(binary, root)
        nominal_key_repairs(binary, root)
    print('Map literals: syntactic constant keys, written effects, spread, bytecode/native and full panic PASS')


if __name__ == '__main__':
    main()
