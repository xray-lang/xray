"""First-use regressions of the public CLI: default stdlib discovery.

The executable under test is the build-tree binary. With --source-tree-stdlib it
is an uninstalled build that falls back to that checkout stdlib; without it the
same relocated layout must fail and name every location it tried."""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

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

    print(f'CLI usability: {passed} commands, stdlib discovery PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
