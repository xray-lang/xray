"""Exercise governed Rune methods through imported scalar generic CTFE closures."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


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
        temporary = tempfile.TemporaryDirectory(prefix='rune-ctfe-', dir=binary.parent)
        output = Path(temporary.name)
    records = []
    try:
        stdlib = output / 'stdlib'
        (stdlib / 'types').mkdir(parents=True)
        poisoned = stdlib / 'types/rune.xr'
        poisoned.write_text('invalid source must not supply the Rune declaration\n', encoding='utf-8')
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
        hidden = dict(env, PATH=str(output / 'no-C-provider'))
        module = ('export fn point(value:rune)->u32 { return value.toUInt32() }\n'
                  'export fn relay(value:rune)->rune { return value }\n'
                  'export fn nested(value:rune)->u32 { return point(relay(value)) }\n'
                  'export fn echo<T>(value:T)->T { return value }\n'
                  'export fn genericPoint(value:rune)->u32 { return echo<rune>(value).toUInt32() }\n'
                  'export fn walk(value:rune,steps:i64)->u32 {\n'
                  ' var current:rune=value\n var remaining:i64=steps\n'
                  ' while (remaining>0) { current=rune(i64(current)+1); remaining=remaining-1 }\n'
                  ' return current.toUInt32()\n}\n')
        (output / 'future-rune.xr').write_text(module, encoding='utf-8')
        prefix = 'import {point,nested,genericPoint,walk} from "./future-rune"\n'
        values = [0, 1, 65, 127, 128, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x1f600, 0x10ffff]
        direct, answer = prefix, bytearray()
        for value in values:
            direct += f'print(comptime point(rune({value})),comptime nested(rune({value})),comptime genericPoint(rune({value})))\n'
            answer.extend((f'{value} {value} {value}\n').encode())
        walks = [(0, 0), (0, 7), (65, 26), (0x1f600, 3), (0x10ffff, 0), (0xd7f0, 14), (0xe000, 1)]
        cells, cell_answer = prefix, bytearray()
        for value, steps in walks:
            assert not 0xd800 <= value + steps <= 0xdfff and value + steps <= 0x10ffff
            cells += f'print(comptime walk(rune({value}),{steps}))\n'
            cell_answer.extend((str(value + steps) + '\n').encode())
        cases = [('direct-nested-generic', direct, bytes(answer)), ('private-cells', cells, bytes(cell_answer))]

        def invoke(name, executable, arguments, environment):
            result = subprocess.run([str(executable), *arguments], cwd=root, env=environment,
                                    capture_output=True, timeout=60)
            (output / (name + '.stdout')).write_bytes(result.stdout)
            (output / (name + '.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name, argv=[str(executable), *arguments], exit_code=result.returncode,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(), stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output / 'commands.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
            return result

        for name, text, expected in cases:
            source = output / (name + '.xr')
            source.write_text(text, encoding='utf-8')
            result = invoke(name + '-vm', binary, ['run', str(source)], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, b''), (name, result)
            generated = output / (name + '.c')
            result = invoke(name + '-c', binary, ['build', '--c-only', str(source), '-o', str(generated)], env)
            assert result.returncode == 0 and not result.stderr, result
            assert b'({' not in generated.read_bytes()
            native = output / (name + '.exe')
            result = invoke(name + '-build', binary, ['build', str(source), '-o', str(native)], env)
            assert result.returncode == 0 and not result.stderr, result
            result = invoke(name + '-native', native, [], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, b''), (name, result)
        poisoned.unlink()
        for name, _, expected in cases:
            result = invoke(name + '-source-absent', binary, ['run', str(output / (name + '.xr'))], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, b''), result
        for name, text, diagnostic in [
                ('float-unadmitted', 'fn rounded(value:f64)->i64 { return value.round() }\nprint(comptime rounded(1.5))\n',
                 b'comptime requires an admitted deterministic scalar direct-call closure'),
                ('owning-text-unadmitted', 'fn text(value:rune)->string { return value.toString() }\nprint(comptime text(rune(128512)))\n',
                 b'comptime result must be a bool, integer, Rune or Unit in this execution domain')]:
            source = output / (name + '.xr')
            source.write_text(text, encoding='utf-8')
            result = invoke(name, binary, ['run', str(source)], hidden)
            assert result.returncode == 1 and not result.stdout, result
            assert diagnostic in result.stderr, result
        (output / 'summary.json').write_text(json.dumps(dict(status='RUNE_CTFE_IMPORT_PASS', commands=len(records),
            scalar_codepoints=len(values), direct_nested_generic_ctfe_calls=len(values)*3, private_cell_cases=len(walks),
            source_poisoned_and_absent=True, VM_C_provider_PATH_hidden=True, library_generic_templates=False,
            user_generic_scalar_instantiation=True, user_aggregate_ctfe=False, float_and_owned_text_ctfe=False,
            library_private_var_opened=False, complete_standard_library=False), indent=2) + '\n', encoding='utf-8')
        print('Rune CTFE: imported scalar generic closures, 13 Unicode boundaries and seven private Cell loops PASS')
        return 0
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
