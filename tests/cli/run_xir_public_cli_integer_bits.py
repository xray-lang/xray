"""Compare all bit recipes with independent whole-word mathematical expectations."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
from xir_no_child_process import NoChildProcess
import subprocess
import tempfile

MASK = (1 << 64) - 1


def signed(bits):
    bits &= MASK
    return bits - (1 << 64) if bits >= (1 << 63) else bits


def oracle(value, count):
    bits, amount = value & MASK, count % 64
    low = (bits & -bits).bit_length() - 1 if bits else 64
    swapped = signed(int.from_bytes(bits.to_bytes(8, 'little'), 'big'))
    # Multiplication and integer division describe the two circular segments.
    left = signed(bits * (1 << amount) + bits // (1 << (64 - amount)))
    right = signed(bits // (1 << amount) + (bits % (1 << amount)) * (1 << (64 - amount)))
    return [bits.bit_count(), 64 - bits.bit_length(), low, swapped, left, right]


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
        temporary = tempfile.TemporaryDirectory(prefix='integer-bits-', dir=binary.parent)
        output = Path(temporary.name)
    records = []
    try:
        process_policy = NoChildProcess(root, output)
        stdlib = output / 'stdlib'
        (stdlib / 'types').mkdir(parents=True)
        declaration = stdlib / 'types/i64.xr'
        declaration.write_text('invalid numeric declaration source\n', encoding='utf-8')
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
        hidden = dict(env, PATH=str(output / 'no-C-provider'))
        values = [0, -1, -(1 << 63), (1 << 63) - 1, 0x0123456789abcdef,
                  signed(0xaaaaaaaaaaaaaaaa), 0x5555555555555555, -42]
        counts = [0, 1, 7, 8, 31, 32, 63, 64, 65, -1, -64, -65, -(1 << 63), (1 << 63) - 1]
        pairs = [(value, count) for value in values for count in counts]
        pairs += [(signed(1 << bit), counts[bit % len(counts)]) for bit in range(64)]
        pairs += [(signed(MASK ^ (1 << bit)), counts[(bit + 1) % len(counts)]) for bit in range(64)]
        body = ('fn row(value:i64,n:i64) {\n'
                ' print(value.popcount(),value.leadingZeros(),value.trailingZeros(),value.byteswap(),value.rotateLeft(n),value.rotateRight(n))\n}\n')
        expected = bytearray()
        for value, count in pairs:
            body += f'row({value},{count})\n'
            expected.extend((' '.join(map(str, oracle(value, count))) + '\n').encode())
        cases = [('word-oracle', body, bytes(expected))]
        methods = ('popcount', 'leadingZeros', 'trailingZeros', 'byteswap', 'rotateLeft', 'rotateRight')
        ctfe = ''
        for method in methods:
            parameter = ',n:i64' if method.startswith('rotate') else ''
            argument = 'n' if parameter else ''
            ctfe += f'fn ct_{method}(value:i64{parameter})->i64 {{ return value.{method}({argument}) }}\n'
        folded = bytearray()
        ctfe_pairs = pairs[:len(values) * len(counts):len(counts)]
        # Negative and endpoint counts also execute the complete CTFE recipe.
        ctfe_pairs += [(1, -1), (-42, -65), (1, -(1 << 63)), (-1, (1 << 63) - 1)]
        for value, count in ctfe_pairs:
            calls = [f'comptime ct_{method}({value}' + (f',{count}' if method.startswith('rotate') else '') + ')'
                     for method in methods]
            ctfe += 'print(' + ','.join(calls) + ')\n'
            folded.extend((' '.join(map(str, oracle(value, count))) + '\n').encode())
        cases.append(('comptime-word', ctfe, bytes(folded)))

        def invoke(name, executable, arguments, environment):
            argv=[str(executable),*arguments]
            restricted=environment is hidden and (not arguments or arguments[0]=='run')
            result=(process_policy.run(name,argv,environment) if restricted else
                    subprocess.run(argv,cwd=root,env=environment,capture_output=True,timeout=60))
            (output / (name + '.stdout')).write_bytes(result.stdout)
            (output / (name + '.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name, argv=[str(executable), *arguments], exit_code=result.returncode,
                                CLI_child_processes_denied=process_policy.supported and restricted,
                                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                                stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output / 'commands.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
            return result

        for name, text, answer in cases:
            source = output / (name + '.xr')
            source.write_text(text, encoding='utf-8')
            result = invoke(name + '-vm', binary, ['run', str(source)], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, answer, b''), (name, result)
            generated = output / (name + '.c')
            result = invoke(name + '-c', binary, ['build', '--c-only', str(source), '-o', str(generated)], env)
            assert result.returncode == 0 and not result.stderr, result
            assert b'({' not in generated.read_bytes()
            native = output / (name + '.exe')
            result = invoke(name + '-build', binary, ['build', str(source), '-o', str(native)], env)
            assert result.returncode == 0 and not result.stderr, result
            result = invoke(name + '-native', native, [], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, answer, b''), (name, result)
        for method in methods:
            argument = '1' if method.startswith('rotate') else ''
            source = output / (method + '-narrow.xr')
            source.write_text(f'var value:u32=1\nprint(value.{method}({argument}))\n', encoding='utf-8')
            result = invoke(method + '-narrow', binary, ['run', str(source)], hidden)
            assert result.returncode == 1 and not result.stdout and result.stderr, result
        for name, text in [('wrong-arity', 'print(1.popcount(0))\n'),
                           ('missing-count', 'print(1.rotateLeft())\n'),
                           ('wrong-count', 'print(1.rotateRight("wrong"))\n'),
                           ('ref-count', 'var n:i64=1\nprint(1.rotateLeft(ref n))\n'),
                           ('extra-type', 'print(1.byteswap<i64>())\n')]:
            source = output / (name + '.xr')
            source.write_text(text, encoding='utf-8')
            result = invoke(name, binary, ['run', str(source)], hidden)
            assert result.returncode == 1 and not result.stdout and result.stderr, result
        declaration.unlink()
        for name, _, answer in cases:
            result = invoke(name + '-source-absent', binary, ['run', str(output / (name + '.xr'))], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, answer, b''), result
        report = output / 'original.json'
        result = invoke('original', binary, ['test', str(root / 'tests/regression/03_operators/0332_int_bits_methods.xr'),
                                          '--report', str(report)], hidden)
        assert result.returncode == 0 and not result.stderr, result
        data = json.loads(report.read_text(encoding='utf-8'))
        assert len(data['files']) == 1, data
        actual = data['files'][0]
        assert actual['tests'] == actual['passed'] == 3, actual
        assert all(actual[key] == 0 for key in ('failed', 'errors', 'timeouts', 'skipped')), actual
        assert len(actual['cases']) == 3 and all(row['status'] == 'passed' for row in actual['cases']), actual
        (output / 'summary.json').write_text(json.dumps(dict(status='INTEGER_BITS_ORACLE_PASS', commands=len(records),
            pairs=len(pairs), methods=6, ctfe_cases=len(ctfe_pairs), ctfe_calls=len(ctfe_pairs)*len(methods),
            all_bit_positions=True, negative_endpoint_counts=True,
            source_absent_and_poisoned=True, VM_C_provider_PATH_hidden=True,
            Windows_absolute_child_denial=process_policy.control, complete_standard_library=False),
            indent=2) + '\n', encoding='utf-8')
        print(f'Integer bit recipes: {len(pairs)} independent word/count pairs, CTFE and original 3 tests PASS')
        return 0
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
