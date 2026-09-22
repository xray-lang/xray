#!/usr/bin/env python3
"""Exercise canonical source execution, C output and native compilation through the CLI."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
import time
from pathlib import Path


def invoke(binary: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )


def invoke_bytes(binary: Path, *arguments: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(binary), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def check_imported_panic_normalization(binary: Path) -> None:
    # The complete import graph is normalized, including uncalled constructors
    # whose narrowed error summaries must not sever constructive error checks.
    with tempfile.TemporaryDirectory(prefix="xray-canonical-import-panic-") as temporary:
        source = Path(temporary) / "main.xr"
        executable = Path(temporary) / "main.exe"
        source.write_text("import datetime\nprint(42)\n", encoding="utf-8")
        vm = invoke_bytes(binary, "run", str(source))
        require((vm.returncode, vm.stdout, vm.stderr) == (0, b"42\n", b""),
                f"import panic normalization VM: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
        built = invoke_bytes(binary, "build", str(source), "-o", str(executable))
        require(built.returncode == 0, f"import panic normalization native: {built.stderr!r}")
        native = invoke_bytes(executable)
        require((native.returncode, native.stdout, native.stderr) == (0, b"42\n", b""),
                f"import panic normalization native: {native.returncode} "
                f"{native.stdout!r} {native.stderr!r}")


def check_structural_module_slots(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-structural-slots-") as temporary:
        root = Path(temporary)
        source = root / "main.xr"
        executable = root / "main.exe"
        (root / "library.xr").write_text(
            "type State = { token:i64, nested:{ text:string }? }\n"
            "var state:State?=null\n"
            "export fn isEmpty() -> bool { return state == null }\n", encoding="utf-8")
        source.write_text('import "./library"\nprint(library.isEmpty())\n', encoding="utf-8")
        vm = invoke_bytes(binary, "run", str(source))
        require((vm.returncode, vm.stdout, vm.stderr) == (0, b"true\n", b""),
                f"structural module slot VM: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
        built = invoke_bytes(binary, "build", str(source), "-o", str(executable))
        require(built.returncode == 0, f"structural module slot native: {built.stderr!r}")
        native = invoke_bytes(executable)
        require((native.returncode, native.stdout, native.stderr) == (0, b"true\n", b""),
                f"structural module slot native: {native.returncode} {native.stdout!r} {native.stderr!r}")


def check_structural_values(binary: Path) -> None:
    cases = [
        ("type State = { token:i64 }\nvar state:State={token:7}\n"
         "var alias=state\nalias.token=42\nprint(state.token)\n", b"42\n"),
        ("type State = { z:i64, text:string, a:bool }\n"
         'var state:State={a:true,text:"before",z:7}\nvar alias=state\n'
         'alias.text="after"\nalias.z=42\nprint(state.z,state.text,state.a)\n', b"42 after true\n"),
        ('import "./library"\nprint(library.bump())\nprint(library.read())\n', b"42 ready\n42 ready\n"),
    ]
    with tempfile.TemporaryDirectory(prefix="xray-structural-values-") as temporary:
        root = Path(temporary)
        (root / "library.xr").write_text(
            'type Child = { text:string }\n'
            'type State = { count:i64, child:Child }\n'
            'var child:Child={text:"start"}\nvar state:State={child:child,count:7}\n'
            'export fn bump() -> string { var alias=state; alias.count=42; '
            'alias.child.text="ready"; return state.count.toString()+" "+state.child.text }\n'
            'export fn read() -> string { return state.count.toString()+" "+state.child.text }\n',
            encoding="utf-8")
        for index, (text, expected) in enumerate(cases):
            source = root / f"main{index}.xr"
            executable = root / f"main{index}.exe"
            source.write_text(text, encoding="utf-8")
            vm = invoke_bytes(binary, "run", str(source))
            require((vm.returncode, vm.stdout, vm.stderr) == (0, expected, b""),
                    f"structural value {index} VM: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
            built = invoke_bytes(binary, "build", str(source), "-o", str(executable))
            require(built.returncode == 0, f"structural value {index} native: {built.stderr!r}")
            native = invoke_bytes(executable)
            require((native.returncode, native.stdout, native.stderr) == (0, expected, b""),
                    f"structural value {index} native: {native.returncode} {native.stdout!r} {native.stderr!r}")


def check_f64_programs(binary: Path) -> None:
    source_text = ('fn identity(value: f64) -> f64 { return value }\n'
                   'const value = identity(1.5)\nconst other = identity(2.25)\n'
                   'print(value == 1.5, value != other, value < other, value <= other)\n'
                   'print(other > value, other >= value, value > other)\n'
                   'const minus = identity(-0.0)\nconst plus = identity(0.0)\n'
                   'print(minus == plus, minus != plus)\n'
                   'print(value, other, minus, plus)\n'
                   'print(1.0, 1e-7, 1e20, 1.2345678901234567)\n'
                   'const counter = Atomic(value)\nprint("value", counter.load(), true)\n'
                   'print(string(value), string(minus), string(true), string(42))\n'
                   'const letter = string(\'界\')\nprint(letter, len(letter))\n'
                   'print(string((-1) as u64), string(255 as u8), string(-128 as i8))\n'
                   'print("v=" + string(value))\n'
                   'print(value.toString(), minus.toString(), true.toString(), (42).toString())\n'
                   'print(((-1) as u64).toString(), (\'界\').toString())\n'
                   'class Label { toString()->string { return "user method" } }\n'
                   'const label = Label()\nprint(label.toString())\n')
    expected = (b"true true true true\ntrue true false\ntrue false\n"
                b"1.5 2.25 -0.0 0.0\n1.0 1e-07 1e+20 1.23456789012346\n"
                b"value 1.5 true\n1.5 -0.0 true 42\n" + "界 1\n18446744073709551615 255 -128\nv=1.5\n1.5 -0.0 true 42\n18446744073709551615 界\nuser method\n".encode())
    with tempfile.TemporaryDirectory(prefix="xray-canonical-f64-") as temporary:
        source = Path(temporary) / "main.xr"
        executable = Path(temporary) / "main.exe"
        source.write_text(source_text, encoding="utf-8")
        vm = invoke_bytes(binary, "run", str(source))
        require((vm.returncode, vm.stdout, vm.stderr) == (0, expected, b""),
                f"f64 VM: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
        built = invoke_bytes(binary, "build", "--native", str(source), "-o", str(executable))
        require(built.returncode == 0, f"f64 native build: {built.stderr!r}")
        native = invoke_bytes(executable)
        require((native.returncode, native.stdout, native.stderr) == (0, expected, b""),
                f"f64 native: {native.returncode} {native.stdout!r} {native.stderr!r}")


def check_atomic_programs(binary: Path) -> None:
    cases = (
        ("cas_f64", 'const counter = Atomic(1.5)\nconst alias = counter\n'
         'var (old, matched) = alias.compareExchange(1.5, 2.25)\n'
         'print(old == 1.5, matched, counter.load() == 2.25)\n'
         'var (failed, rejected) = counter.compareExchange(1.5, 9.0)\n'
         'print(failed == 2.25, rejected, alias.load() == 2.25)\n',
         b"true true true\ntrue false true\n"),
        ("cas_f64_zero", 'const counter = Atomic(-0.0)\n'
         'var (old, matched) = counter.compareExchange(0.0, 2.25)\n'
         'print(old == 0.0, matched, counter.load() == 0.0)\n'
         'var (before, accepted) = counter.compareExchange(-0.0, 1.5)\n'
         'print(before == 0.0, accepted, counter.load() == 1.5)\n',
         b"true false true\ntrue true true\n"),
        *(("f64_ordering_" + order,
           'const counter = Atomic(1.5)\nconst alias = counter\n'
           f'print(alias.fetchAdd(2.25, Ordering.{order}) == 1.5)\n'
           f'print(counter.load(Ordering.{order}) == 3.75)\n'
           f'counter.sub(2.25, Ordering.{order})\n'
           f'print(alias.swap(2.25, Ordering.{order}) == 1.5)\n'
           f'counter.store(-0.0, Ordering.{order})\n'
           f'print(alias.load(Ordering.{order}) == 0.0)\n',
           b"true\ntrue\ntrue\ntrue\n")
          for order in ("Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst")),
        *( ("ordering_" + order,
             'const counter = Atomic(40)\nconst alias = counter\n'
             f'alias.store(42, Ordering.{order})\n'
             f'print(counter.load(Ordering.{order}))\n'
             f'print(alias.fetchAdd(2, Ordering.{order}))\n'
             f'counter.sub(2, Ordering.{order})\n'
             f'var (old, matched) = counter.compareExchange(42, 7, Ordering.{order})\n'
             f'print(old, matched, alias.swap(9, Ordering.{order}))\n'
             'const flag = Atomic(false)\n'
             f'print(flag.toggle(Ordering.{order}), flag.load(Ordering.{order}))\n',
             b"42\n42\n42 true 7\nfalse true\n")
           for order in ("Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst") ),
        ("store_i64", 'const counter = Atomic(40)\nconst alias = counter\n'
         'alias.store(42)\nprint(counter.load())\ncounter.store(-7)\nprint(alias.load())\n',
         b"42\n-7\n"),
        ("store_bool", 'const flag = Atomic(false)\nconst alias = flag\n'
         'alias.store(true)\nprint(flag.load())\nflag.store(false)\nprint(alias.load())\n',
         b"true\nfalse\n"),
        ("void_updates", 'const counter = Atomic(40)\nconst alias = counter\n'
         'alias.add(4)\nprint(counter.load())\ncounter.sub(2)\nprint(alias.load())\n', b"44\n42\n"),
        ("fetch_sub", 'const counter = Atomic(42)\nconst alias = counter\n'
         'print(alias.fetchSub(2), counter.load())\n'
         'print(counter.fetchSub(-2), alias.load())\n', b"42 40\n40 42\n"),
        ("construct_i64", 'const counter = Atomic(40)\nconst alias = counter\n'
         'print(42)\n', b"42\n"),
        ("construct_bool", 'const flag = Atomic(false)\nconst alias = flag\n'
         'print(true)\n', b"true\n"),
        ("exchange_i64", 'const counter = Atomic(40)\nconst alias = counter\n'
         'print(alias.swap(42), counter.load())\n', b"40 42\n"),
        ("exchange_bool", 'const flag = Atomic(false)\nconst alias = flag\n'
         'print(alias.swap(true), flag.load())\n', b"false true\n"),
        ("cas_i64", 'const counter = Atomic(40)\nconst alias = counter\n'
         'var (old, matched) = alias.compareExchange(40, 42)\n'
         'print(old, matched, counter.load())\n'
         'var (failed, rejected) = counter.compareExchange(40, 99)\n'
         'print(failed, rejected, alias.load())\n', b"40 true 42\n42 false 42\n"),
        ("cas_bool", ''.join(
            f'const flag{i} = Atomic({initial})\n'
            f'const alias{i} = flag{i}\n'
            f'var (old{i}, matched{i}) = alias{i}.compareExchange({expected}, {desired})\n'
            f'print(old{i}, matched{i}, flag{i}.load())\n'
            for i, (initial, expected, desired) in enumerate((
                ("false", "false", "true"), ("false", "true", "true"),
                ("true", "false", "false"), ("true", "true", "false")))),
         b"false true true\nfalse false false\ntrue false true\ntrue true false\n"),
        ("i64", 'const counter = Atomic(40)\nconst alias = counter\n'
         'print(alias.fetchAdd(2), counter.load())\nprint(counter.swap(7))\n'
         'var (old, matched) = alias.compareExchange(7, 9)\n'
         'print(old, matched, counter.load())\n', b"40 42\n42\n7 true 9\n"),
        ("bool", 'const flag = Atomic(false)\nconst alias = flag\nprint(flag.load())\n'
         'print(alias.toggle(), flag.load())\n'
         'var (old, matched) = flag.compareExchange(true, false)\n'
         'print(old, matched, alias.load())\n', b"false\nfalse true\ntrue true false\n"),
        ("f64", 'const counter = Atomic(1.5)\nconst alias = counter\n'
         'const old = alias.fetchAdd(2.25)\n'
         'print(old == 1.5, counter.load() == 3.75)\n'
         'counter.sub(2.25)\nprint(alias.load() == 1.5)\n', b"true true\ntrue\n"),
    )
    failures = []
    with tempfile.TemporaryDirectory(prefix="xray-canonical-atomic-") as temporary:
        for name, program, expected in cases:
            source = Path(temporary) / f"{name}.xr"
            executable = Path(temporary) / f"{name}.exe"
            source.write_text(program, encoding="utf-8")
            vm = invoke_bytes(binary, "run", str(source))
            if (vm.returncode, vm.stdout, vm.stderr) != (0, expected, b""):
                failures.append(f"Atomic<{name}> VM: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
            built = invoke_bytes(binary, "build", "--native", str(source), "-o", str(executable))
            if built.returncode != 0:
                failures.append(f"Atomic<{name}> native build: {built.stderr!r}")
                continue
            native = invoke_bytes(executable)
            if (native.returncode, native.stdout, native.stderr) != (0, expected, b""):
                failures.append(f"Atomic<{name}> native: {native.returncode} "
                                f"{native.stdout!r} {native.stderr!r}")
    require(not failures, "\n".join(failures))


def check_typed_error_reports(binary: Path) -> None:
    long_message = "detail-" + "x" * 4096
    cases = (
        ('enum TopErr { Failed { reason: string } }\n'
         'fn fail() { throw TopErr.Failed { reason: "top-level" } }\n'
         'print("before"); fail()\n',
         b"before\n", b'[Uncaught Error] TopErr.Failed("top-level")\n', 1),
        ('import time\nenum TopErr { Failed { reason: string } }\n'
         'fn fail() { defer { print("cleanup") }; time.sleep(1); '
         'throw TopErr.Failed { reason: "after-wait" } }\nfail()\n',
         b"cleanup\n", b'[Uncaught Error] TopErr.Failed("after-wait")\n', 1),
        ('enum Detail { Message { text: string } }\n'
         'enum Failure { Failed { detail: Detail, code: i64, ok: bool, letter: rune } }\n'
         'fn fail() { throw Failure.Failed { '
         f'detail: Detail.Message {{ text: "{long_message}" }}, '
         "code: -7, ok: true, letter: 'x' } }\nfail()\n",
         b"", (f'[Uncaught Error] Failure.Failed(Detail.Message("{long_message}"), '
          "-7, true, 'x')\n").encode(), 1),
        ('enum TopErr { Failed { reason: string } }\n'
         'fn fail() { throw TopErr.Failed { reason: "caught" } }\n'
         'try { fail() } catch (e) { print("caught") }\n',
         b"caught\n", b"", 0),
        ('import time\nenum TopErr { Failed { reason: string } }\n'
         'fn fail() { defer { print("cleanup") }; '
         'throw TopErr.Failed { reason: "owned" + " string" } }\n'
         'fn waited() { time.sleep(1); fail() }\n'
         'try { fail() } catch (e) { print("caught first") }\n'
         'try { waited() } catch (e) { print("caught second") }\n',
         b"cleanup\ncaught first\ncleanup\ncaught second\n", b"", 0),
    )
    with tempfile.TemporaryDirectory(prefix="xray-canonical-errors-") as temporary:
        source = Path(temporary) / "main.xr"
        executable = Path(temporary) / "error.exe"
        for program, stdout, stderr, exit_code in cases:
            source.write_text(program, encoding="utf-8")
            vm = invoke_bytes(binary, "run", str(source))
            require((vm.returncode, vm.stdout, vm.stderr) == (exit_code, stdout, stderr),
                    f"VM lost original error: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
            built = invoke(binary, "build", str(source), "-o", str(executable))
            require(built.returncode == 0, f"native error build failed: {built.stdout} {built.stderr}")
            native = invoke_bytes(executable)
            require((native.returncode, native.stdout, native.stderr) == (exit_code, stdout, stderr),
                    f"native lost original error: {native.returncode} {native.stdout!r} {native.stderr!r}")


def check_integer_output(binary: Path) -> None:
    values = (
        ("i8", "-128", "127"), ("u8", "0", "255"),
        ("i16", "-32768", "32767"), ("u16", "0", "65535"),
        ("i32", "-2147483648", "2147483647"), ("u32", "0", "4294967295"),
        ("i64", "-9223372036854775808", "9223372036854775807"),
        ("u64", "0", "18446744073709551615"),
    )
    declarations = "\n".join(f"var lo_{t}:{t} = {low}; var hi_{t}:{t} = {high}"
                              for t, low, high in values)
    arguments = ", ".join(f"lo_{t}, hi_{t}" for t, _, _ in values)
    expected = (b"-128 127 0 255 -32768 32767 0 65535 -2147483648 2147483647 "
                b"0 4294967295 -9223372036854775808 9223372036854775807 "
                b"0 18446744073709551615 true end\n")
    with tempfile.TemporaryDirectory(prefix="xray-integer-output-") as temporary:
        source = Path(temporary) / "main.xr"
        executable = Path(temporary) / "integers.exe"
        source.write_text('import time\nfn display() {\n' + declarations + '\n'
                          'var text = "e" + "nd"\n'
                          f'print({arguments}, true, text)\n'
                          'time.sleep(1)\n'
                          f'print({arguments}, true, text)\n'
                          '}\ndisplay()\n', encoding="utf-8")
        vm = invoke_bytes(binary, "run", str(source))
        require((vm.returncode, vm.stdout, vm.stderr) == (0, expected * 2, b""),
                f"VM integer output failed: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
        built = invoke(binary, "build", str(source), "-o", str(executable))
        require(built.returncode == 0, f"native integer output build failed: {built.stderr}")
        native = invoke_bytes(executable)
        require((native.returncode, native.stdout, native.stderr) == (0, expected * 2, b""),
                f"native integer output failed: {native.returncode} {native.stdout!r} {native.stderr!r}")


def check_string_lengths(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-scalar-length-") as temporary:
        directory = Path(temporary)
        (directory / "helper.xr").write_text(
            'export fn text()->string { return "世" + "😀" }\n'
            'export class Label {\n  text:string\n'
            '  constructor(s:string) {this.text=s}\n'
            '  length()->i64 {return len(this.text)}\n}\n'
            'var saved=Label("世😀")\n'
            'export fn storedLength()->i64 {return saved.length()}\n', encoding="utf-8")
        source = directory / "main.xr"
        source.write_text(
            'import time\nimport {text, Label, storedLength} from "./helper"\n'
            'fn read(s:string)->i64 {return len(s)}\n'
            'fn exercise() {\n'
            'var a=text(); var copied=copy(a); var label=Label(a)\n'
            'a="changed"; var joined=copied + "é"\n'
            'print(len(""), len("abc"), len("é"), len("世"), len("😀"), len("é"))\n'
            'print(read(copied), label.length(), len(joined), len(string(-123)))\n'
            'time.sleep(1)\n'
            'print(read(copied), label.length(), len(joined), len(a))\n'
            'print(storedLength())\n'
            '}\nexercise()\n', encoding="utf-8")
        expected = b"0 3 1 1 1 2\n2 2 4 4\n2 2 4 7\n2\n"
        vm = invoke_bytes(binary, "run", str(source))
        require((vm.returncode, vm.stdout, vm.stderr) == (0, expected, b""),
                f"VM scalar length failed: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
        executable = directory / "length.exe"
        built = invoke(binary, "build", str(source), "-o", str(executable))
        require(built.returncode == 0, f"native scalar length build failed: {built.stderr}")
        native = invoke_bytes(executable)
        require((native.returncode, native.stdout, native.stderr) == (0, expected, b""),
                f"native scalar length failed: {native.returncode} {native.stdout!r} {native.stderr!r}")


def check_arrays(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="xray-array-values-") as temporary:
        directory = Path(temporary)
        (directory / "helper.xr").write_text(
            'var saved = [["module", "世"], ["other"]]\n'
            'export fn storedLength()->i64 {return len(saved)}\n', encoding="utf-8")
        source = directory / "main.xr"
        source.write_text(
            'import time\nimport {storedLength} from "./helper"\n'
            'fn read(a:Array<string>)->i64 {return len(a)}\n'
            'fn exercise() {\n'
            'var empty:Array<string> = []; var emptyCopy=copy(empty)\n'
            'var numbers=[1, 2, 3]; var numbersCopy=copy(numbers)\n'
            'var text="世"; var words=[text, text+"😀"]; var copied=copy(words)\n'
            'var nested=[words, copied]; var nestedCopy=copy(nested)\n'
            'words=["replacement"]; nested=[]; text="changed"\n'
            'print(len(emptyCopy), len(numbersCopy), read(copied), len(nestedCopy))\n'
            'time.sleep(1)\n'
            'print(read(copied), len(words), len(nestedCopy), storedLength())\n'
            '}\nexercise()\nexercise()\n', encoding="utf-8")
        expected = b"0 3 2 2\n2 1 2 2\n" * 2
        vm = invoke_bytes(binary, "run", str(source))
        require((vm.returncode, vm.stdout, vm.stderr) == (0, expected, b""),
                f"VM arrays failed: {vm.returncode} {vm.stdout!r} {vm.stderr!r}")
        executable = directory / "arrays.exe"
        built = invoke(binary, "build", str(source), "-o", str(executable))
        require(built.returncode == 0, f"native arrays build failed: {built.stderr}")
        native = invoke_bytes(executable)
        require((native.returncode, native.stdout, native.stderr) == (0, expected, b""),
                f"native arrays failed: {native.returncode} {native.stdout!r} {native.stderr!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--fixtures", required=True, type=Path)
    parser.add_argument("--compile-errors", required=True, type=Path)
    args = parser.parse_args()
    check_integer_output(args.binary)
    check_string_lengths(args.binary)
    check_arrays(args.binary)

    zero = invoke(args.binary, "run", str(args.fixtures / "main_zero.xr"))
    require(zero.returncode == 0, f"zero initializer failed: {zero.stderr}")
    require(zero.stdout == "", f"zero initializer produced stdout: {zero.stdout!r}")

    printed = invoke_bytes(args.binary, "run", str(args.fixtures / "main_print.xr"))
    require(printed.returncode == 0, f"print initializer failed: {printed.stderr!r}")
    require(printed.stdout == b"42\n", f"initializer output bytes drifted: {printed.stdout!r}")

    clock = invoke(args.binary, "run", str(args.fixtures / "time_now.xr"))
    require(clock.returncode == 0, f"clock provider route failed: {clock.stderr!r}")
    clock_text = clock.stdout.strip()
    require(
        clock_text.isdecimal() and int(clock_text) > 0,
        f"clock provider did not produce positive epoch milliseconds: {clock.stdout!r}",
    )

    sleep_started = time.monotonic()
    slept = invoke(args.binary, "run", str(args.fixtures / "time_sleep.xr"))
    sleep_elapsed = time.monotonic() - sleep_started
    require(slept.returncode == 0, f"timer suspension route failed: {slept.stderr!r}")
    require(
        slept.stdout == "",
        f"timer suspension produced unexpected stdout: {slept.stdout!r}",
    )
    require(
        sleep_elapsed >= 0.020,
        f"canonical run did not wait for timer readiness: {sleep_elapsed:.6f}s",
    )

    implicit = invoke(args.binary, str(args.fixtures / "main_zero.xr"))
    require(implicit.returncode == 0, f"implicit source run failed: {implicit.stderr}")

    with tempfile.TemporaryDirectory(prefix="xray-canonical-crlf-") as temporary:
        crlf = Path(temporary) / "main.xr"
        crlf.write_bytes(b"fn main() -> i64 {\r\n    return 0\r\n}\r\n")
        normalized = invoke(args.binary, "run", str(crlf))
        require(
            normalized.returncode == 0,
            f"source fingerprint drifted across CRLF ingestion: {normalized.stderr}",
        )

    declaration_only = invoke(
        args.binary, "run", str(args.fixtures / "declaration_only.xr")
    )
    require(
        declaration_only.returncode == 0,
        f"declaration-only module initializer failed: {declaration_only.stderr}",
    )

    retired = invoke(
        args.binary,
        "run",
        str(args.fixtures / "main_zero.xr"),
        "--semantic-plan",
        "removed.xtp",
    )
    require(retired.returncode != 0, "retired TargetPlan option was accepted")
    require(
        "unknown option '--semantic-plan'" in retired.stderr,
        f"retired option did not fail in the CLI parser: {retired.stderr!r}",
    )

    stdin = invoke(args.binary, "run", "-")
    require(stdin.returncode != 0, "source without file authority was accepted")
    require(
        "XR_RUN_6011: canonical run requires a file-backed source authority" in stdin.stderr,
        f"file authority rejection drifted: {stdin.stderr!r}",
    )

    artifact = invoke(args.binary, "run", str(args.fixtures / "retired.xtp"))
    require(artifact.returncode != 0, "retired artifact path was accepted")
    require(
        "XR_RUN_6013: canonical run accepts only an exact '.xr' source path"
        in artifact.stderr,
        f"retired artifact path did not stop at the source boundary: {artifact.stderr!r}",
    )

    mono_depth = args.compile_errors / "005_mono_depth_budget.xr"
    rejected_run = invoke(args.binary, "run", str(mono_depth))
    require(rejected_run.returncode != 0, "run accepted unbounded generic specialization")
    require(
        ":1033:0: error[E0389]: E0389:" in rejected_run.stderr,
        f"run lost the exact monomorphization budget failure: {rejected_run.stderr!r}",
    )
    rejected_check = invoke(args.binary, "check", str(mono_depth))
    require(rejected_check.returncode != 0, "check accepted unbounded generic specialization")
    require(
        ": error: E0389:" in rejected_check.stderr,
        f"check lost the exact monomorphization budget failure: {rejected_check.stderr!r}",
    )

    with tempfile.TemporaryDirectory(prefix="xray-canonical-emit-") as temporary:
        output = Path(temporary) / "program.c"
        for source in (args.fixtures / "main_print.xr",
                       args.fixtures.parent / "module-report" / "main.xr",
                       args.fixtures.parent / "module-report" / "extended.xr"):
            emitted = invoke(args.binary, "build", "--c-only", str(source),
                             "-o", str(output))
            require(emitted.returncode == 0, f"canonical C emission failed: {emitted.stderr}")
            original = output.read_bytes()
            require(b"xr_aot_entry_coroutine_descriptor" in original,
                    "C output lacks the canonical native descriptor")
            require(b"XrProto" not in original and b"xrt_vm_run" not in original,
                    "C output regained the old VM route")
            repeated = invoke(args.binary, "build", "--native", "--c-only", str(source),
                              "-o", str(output))
            require(repeated.returncode == 0 and output.read_bytes() == original,
                    f"canonical C emission is not deterministic: {repeated.stderr}")

        output.write_bytes(b"existing output must survive source rejection")
        rejected_emit = invoke(args.binary, "build", "--native", "--c-only", str(mono_depth),
                               "-o", str(output))
        require(rejected_emit.returncode != 0 and "E0389" in rejected_emit.stderr,
                f"C emission lost source admission: {rejected_emit.stderr}")
        require(output.read_bytes() == b"existing output must survive source rejection",
                "failed source admission overwrote the existing C output")

    with tempfile.TemporaryDirectory(prefix="xray-canonical-native-") as temporary:
        executable = Path(temporary) / "program.exe"
        programs = (
            (args.fixtures / "main_print.xr", b"42\n"),
            (args.fixtures.parent / "module-report" / "main.xr",
             b"library 40 ready\nentry report\nalpha:41 beta:42 42\n"),
            (args.fixtures.parent / "module-report" / "extended.xr",
             b"library 40 ready\nfacade warm:41\nentry report\nrow:42|row:43 tail:44 44\n"),
        )
        for source, expected in programs:
            built = invoke(args.binary, "build", str(source), "-o", str(executable))
            require(built.returncode == 0, f"native CLI build failed: {built.stdout} {built.stderr}")
            executed = invoke_bytes(executable)
            require(executed.returncode == 0 and executed.stdout == expected,
                    f"native independent output failed: {executed.stdout!r} {executed.stderr!r}")
        pipe_source = Path(temporary) / "pipe.xr"
        pipe_source.write_text(
            "import sys\n"
            "fn answer() -> i64 {\n"
            "  var opened = sys.Pipe.open()\n"
            "  if (opened == null) { return 0 }\n"
            "  var ready = opened!\n"
            "  var live = move ready\n"
            "  var readClosed = live.closeRead()\n"
            "  var writeClosed = live.closeWrite()\n"
            "  var closed = (move live).close()\n"
            "  if (!readClosed || !writeClosed || !closed) { return 0 }\n"
            "  return 1\n"
            "}\nprint(answer())\n", encoding="utf-8")
        built = invoke(args.binary, "build", "--native", str(pipe_source), "-o", str(executable))
        require(built.returncode == 0, f"native Pipe build failed: {built.stdout} {built.stderr}")
        executed = invoke_bytes(executable)
        require(executed.returncode == 0 and executed.stdout == b"1\n",
                f"native Pipe open/close failed: {executed.stdout!r} {executed.stderr!r}")
        for name in ("time_now.xr", "time_sleep.xr"):
            built = invoke(args.binary, "build", "--native", str(args.fixtures / name),
                           "-o", str(executable))
            require(built.returncode == 0, f"native provider build failed: {built.stdout} {built.stderr}")
            started = time.monotonic()
            executed = invoke(executable)
            require(executed.returncode == 0, f"native provider execution failed: {executed.stderr}")
            if name == "time_now.xr":
                require(executed.stdout.strip().isdecimal() and int(executed.stdout.strip()) > 0,
                        f"native clock did not produce positive epoch milliseconds: {executed.stdout!r}")
            else:
                require(executed.stdout == "" and time.monotonic() - started >= 0.020,
                        "native timer did not wait for readiness")
        sentinel = executable.read_bytes()
        rejected = invoke(args.binary, "build", "--native", str(mono_depth), "-o", str(executable))
        require(rejected.returncode != 0 and "E0389" in rejected.stderr,
                f"native build lost source admission: {rejected.stderr}")
        require(executable.read_bytes() == sentinel, "source rejection overwrote native output")
        preview = invoke(args.binary, "build", "--native", "--dry-run-link",
                         str(args.fixtures / "main_print.xr"), "-o", str(executable))
        require(preview.returncode == 0 and "Link command" in preview.stdout,
                f"native link preview failed: {preview.stderr}")
        require(executable.read_bytes() == sentinel, "dry-run link overwrote native output")

    check_imported_panic_normalization(args.binary)
    check_structural_module_slots(args.binary)
    check_structural_values(args.binary)
    check_typed_error_reports(args.binary)
    check_f64_programs(args.binary)
    check_atomic_programs(args.binary)
    print("canonical source run, C output and native executable routes passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
