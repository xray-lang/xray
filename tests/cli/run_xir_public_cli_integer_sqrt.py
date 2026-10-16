"""Check integer square roots against exact binary64 midpoint squares."""
from __future__ import annotations
import argparse
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from xir_no_child_process import NoChildProcess


def bits(value):
    return struct.unpack('>Q', struct.pack('>d', value))[0]


def floating(payload):
    return struct.unpack('>d', struct.pack('>Q', payload))[0]


def expected(value):
    if value < 0:
        return None
    target = Fraction.from_float(float(value))
    lo, hi = 0, bits(4294967296.0)
    # Searching encodings and comparing rational midpoint squares is independent
    # of the restoring integer algorithm used by the compiler's recipe.
    while hi-lo > 1:
        mid = (lo+hi)//2
        f = Fraction.from_float(floating(mid))
        if f*f <= target:
            lo = mid
        else:
            hi = mid
    a, b = (Fraction.from_float(floating(p)) for p in (lo, hi))
    midpoint = (a+b)/2
    return hi if target > midpoint*midpoint or (target == midpoint*midpoint and lo&1) else lo


def cases():
    values = {-(1<<63), -1, 0, 1, 2, 3, 4, (1<<63)-1}
    for e in range(63):
        for delta in range(-3, 4):
            values.add(max(0, min((1<<63)-1, (1<<e)+delta)))
    for root in [3, 7, 255, 65535, (1<<26)-1, (1<<27)+1, 3037000499]:
        values.update(root*root+d for d in range(-3, 4))
    rng = random.Random(325)
    values.update(rng.randrange(1<<63) for _ in range(128))
    return sorted(values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    binary.relative_to(root)
    temporary = None
    if args.output:
        output = args.output.resolve()
        output.relative_to(root)
        output.mkdir(parents=True, exist_ok=False)
    else:
        temporary = tempfile.TemporaryDirectory(prefix='integer-sqrt-', dir=binary.parent)
        output = Path(temporary.name)
    records = []
    try:
        policy = NoChildProcess(root, output)
        stdlib = output/'stdlib'
        (stdlib/'types').mkdir(parents=True)
        declaration = stdlib/'types/i64.xr'
        declaration.write_text('invalid source must not supply square roots\n', encoding='utf-8')
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
        hidden = dict(env, PATH=str(output/'no-C-provider'))
        (output/'future-sqrt.xr').write_text(
            'export fn root(value:i64)->f64{return value.sqrt()}\n'
            'export fn nested(value:i64)->f64{return identity<f64>(value.sqrt())}\n'
            'fn identity<T>(value:T)->T{return value}\n', encoding='utf-8')
        prefix = 'import {root,nested} from "./future-sqrt"\n'
        ordinary = cases()
        folded = [-(1<<63), -1, 0, 1, 2, 3, 4, (1<<53)-1, (1<<53)+1, (1<<63)-1]

        def invoke(name, executable, arguments, environment, execute=False):
            argv = [str(executable), *map(str, arguments)]
            result = policy.run(name, argv, environment) if execute else subprocess.run(
                argv, cwd=root, env=environment, capture_output=True, timeout=120)
            (output/(name+'.stdout')).write_bytes(result.stdout)
            (output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name, argv=argv, exit_code=result.returncode,
                child_processes_denied=execute and policy.supported,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
            return result

        def validate(result, inputs):
            assert result.returncode == 0 and not result.stderr, result
            lines = result.stdout.splitlines()
            assert len(lines) == len(inputs), (len(lines), len(inputs))
            for value, line in zip(inputs, lines):
                actual = float(line)
                if value < 0:
                    assert math.isnan(actual), (value, line)
                else:
                    assert bits(actual) == expected(value), (value, line, expected(value))

        for name, inputs, comptime in [('ordinary', ordinary, False), ('ctfe', folded, True)]:
            source = output/(name+'.xr')
            source.write_text(prefix+''.join(f'print({"comptime nested" if comptime else "root"}({v}))\n'
                                            for v in inputs), encoding='utf-8')
            validate(invoke(name+'-vm', binary, ['run', source], hidden, True), inputs)
            generated = output/(name+'.c')
            result = invoke(name+'-c', binary, ['build', '--c-only', source, '-o', generated], env)
            assert result.returncode == 0 and not result.stderr and b'({' not in generated.read_bytes(), result
            native = output/(name+'.exe')
            result = invoke(name+'-build', binary, ['build', source, '-o', native], env)
            assert result.returncode == 0 and not result.stderr, result
            validate(invoke(name+'-native', native, [], hidden, True), inputs)
            declaration.unlink()
            validate(invoke(name+'-absent', binary, ['run', source], hidden, True), inputs)
            declaration.write_text('poisoned declaration\n', encoding='utf-8')
        negatives = [('arity', 'const n:i64=2\nprint(n.sqrt(1))\n'),
                     ('narrow', 'const n:i8=2\nprint(n.sqrt())\n')]
        for name, text in negatives:
            source = output/(name+'.xr')
            source.write_text(text, encoding='utf-8')
            for mode in ('run', 'build'):
                arguments = [mode, source] if mode == 'run' else [mode, '--c-only', source, '-o', output/(name+'.c')]
                result = invoke(name+'-'+mode, binary, arguments, hidden, mode == 'run')
                assert result.returncode != 0 and not result.stdout, result
                assert b'integer operation requires its exact ordinary declaration' in result.stderr, result
        result = dict(state='PASS', ordinary_cases=len(ordinary), ctfe_cases=len(folded),
                      negative_cases=len(negatives), commands=len(records),
                      oracle='exact rational binary64 midpoint squares',
                      binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
        (output/'validation.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(result))
        return 0
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
