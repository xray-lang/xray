"""First-use regressions of the public CLI: default stdlib discovery and BOM sources.

The executable under test is the build-tree binary. With --source-tree-stdlib it
is an uninstalled build that falls back to that checkout stdlib; without it the
same relocated layout must fail and name every location it tried."""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

BOM = b'\xef\xbb\xbf'
HINT = b'  hint: set XRAY_STDLIB_PATH to the standard library directory\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-tree-stdlib', type=Path)
    args = parser.parse_args()
    binary, root, output = args.binary.resolve(), args.root.resolve(), args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    base_env = {key: value for key, value in os.environ.items() if key != 'XRAY_STDLIB_PATH'}
    base_env['NO_COLOR'] = '1'
    stdlib_env = dict(base_env, XRAY_STDLIB_PATH=str(root / 'stdlib'))
    passed = 0

    def run(name, command, cwd, env, expected):
        nonlocal passed
        result = subprocess.run([str(part) for part in command], cwd=cwd, env=env,
                                capture_output=True, timeout=120)
        (output / f'{name}.stdout').write_bytes(result.stdout)
        (output / f'{name}.stderr').write_bytes(result.stderr)
        actual = (result.returncode, result.stdout, result.stderr)
        if expected is not None and actual != expected:
            raise AssertionError(f'{name}: {command!r}\nexpected {expected!r}\nactual   {actual!r}')
        passed += 1
        return result

    # A relocated copy has neither <exe>/../stdlib nor <exe>/../lib/xray/stdlib,
    # and its working directory has no stdlib: only the last candidate remains.
    home = output / 'relocated'
    (home / 'bin').mkdir(parents=True)
    work = home / 'work'
    work.mkdir()
    relocated = home / 'bin' / binary.name
    shutil.copyfile(binary, relocated)
    hello = work / 'hello.xr'
    hello.write_bytes(b'print("hello")\n')
    tried = [home / 'stdlib', home / 'lib' / 'xray' / 'stdlib', work / 'stdlib']
    if args.source_tree_stdlib:
        run('default-stdlib-run', [relocated, 'run', hello], work, base_env, (0, b'hello\n', b''))
        published = work / 'hello.c'
        run('default-stdlib-build-c', [relocated, 'build', '-c', hello, '-o', published], work, base_env,
            (0, f'Generated: {published}\n'.encode(), b''))
        assert published.stat().st_size > 0
    else:
        listing = b''.join(b'    ' + str(path).encode() + b'\n' for path in tried)
        run('default-stdlib-missing', [relocated, 'run', hello], work, base_env,
            (1, b'', b'XR_RUN_6001: source path lookup failed (stage=4 status=not-found io=2)\n'
                     b'  standard library locations tried:\n' + listing + HINT))
    # An explicit override is mandatory: it is the only location named.
    missing = home / 'missing-stdlib'
    run('override-missing', [relocated, 'build', '-c', hello, '-o', work / 'unused.c'], work,
        dict(base_env, XRAY_STDLIB_PATH=str(missing)),
        (1, b'', b'XR_BUILD_6003: source path lookup failed (stage=4 status=not-found io=2)\n'
                 b'  standard library locations tried:\n    ' + str(missing).encode() + b'\n' + HINT))
    assert not (work / 'unused.c').exists()

    # A leading UTF-8 BOM is skipped by run and by build -c; columns exclude it.
    bom = output / 'bom'
    bom.mkdir()
    program = b'print("bom")\n'
    source = bom / 'main.xr'
    source.write_bytes(BOM + program)
    run('bom-run', [binary, 'run', source], bom, stdlib_env, (0, b'bom\n', b''))
    # The same path with and without the mark must project identical C.
    for name, text in (('bom', BOM + program), ('plain', program)):
        source.write_bytes(text)
        published = bom / f'{name}.c'
        run(f'{name}-build-c', [binary, 'build', '-c', source, '-o', published], bom, stdlib_env,
            (0, f'Generated: {published}\n'.encode(), b''))
    assert (bom / 'bom.c').read_bytes() == (bom / 'plain.c').read_bytes()
    located = bom / 'located.xr'
    located.write_bytes(BOM + b'var value = missing_symbol\n')
    run('bom-column', [binary, 'run', located], bom, stdlib_env,
        (1, b'', str(located).encode() + b':1:13: error: name is not an initialized value\n'))
    # Only the leading mark is a BOM; U+FEFF later in the file is not skipped.
    interior = bom / 'interior.xr'
    interior.write_bytes(b'print(1)\n' + BOM + b'print(2)\n')
    rejected = run('bom-interior', [binary, 'run', interior], bom, stdlib_env, None)
    assert rejected.returncode == 1 and rejected.stdout == b'' and b':2:1: error:' in rejected.stderr, rejected

    print(f'CLI usability: {passed} commands, stdlib discovery and BOM sources PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
