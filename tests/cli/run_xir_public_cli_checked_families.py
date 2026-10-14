"""Exercise a published private library closure without source or a VM C provider."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
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
        temporary = tempfile.TemporaryDirectory(prefix='checked-families-', dir=binary.parent)
        output = Path(temporary.name).resolve()
    records = []
    try:
        stdlib = output / 'stdlib'
        (stdlib / 'io').mkdir(parents=True)
        poison = stdlib / 'io/output.xr'
        poison.write_text('invalid library source must not be parsed\n', encoding='utf-8')
        (stdlib / 'time').mkdir()
        time_poison = stdlib / 'time/time.xr'
        time_poison.write_text('invalid clock source must not be parsed\n', encoding='utf-8')
        poison_digest = hashlib.sha256(poison.read_bytes()).hexdigest()
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR='1')
        hidden = dict(env, PATH=str(output / 'no-C-provider'))
        cases = [
            ('nominal', 'import "std/io/output" as Lib\nenum User { Ready, Done }\n'
             'fn emit<T>(value:T)->T { Lib.writeStdout("A\\0B"); return value }\n'
             'print(match(emit(User.Ready)){User.Ready->73,User.Done->0})\n'
             'print(emit(7))\n', b'A\0B73\nA\0B7\n'),
            ('values', 'import "std/io/output" as Lib\n'
             'fn emit<T>(value:T)->T { Lib.writeStdout("+"); return value }\n'
             'print(emit(false),emit(1.5))\n'
             'const values=emit([11,22])\nprint(values[0],values[1])\n'
             'var original=[11,22]\nvar returned=emit(original)\nreturned[0]=99\n'
             'print(original[0],returned[0])\n', b'++false 1.5\n+11 22\n+11 99\n'),
            ('clocks', 'import time as Lib\n'
             'const first=Lib.nanos()\nLib.sleep(-1)\nLib.sleep(0)\nLib.sleep(1)\n'
             'const last=Lib.nanos()\nconst micro=Lib.micros()\nconst milli=Lib.monotonic()\n'
             'const cpu=Lib.clock()\nconst nextCpu=Lib.clock()\n'
             'const offset=Lib.localOffset()\nconst oldOffset=Lib.localOffsetAt(0)\n'
             'print(last-first>=1000000,micro/1000<=milli,nextCpu>=cpu,Lib.now()>0,\n'
             'offset>=-1440 && offset<=1440,oldOffset>=-1440 && oldOffset<=1440)\n',
             b'true true true true true true\n'),
        ]

        def invoke(name: str, executable: Path, arguments: list[str], environment: dict) -> subprocess.CompletedProcess:
            argv = [str(executable), *arguments]
            result = subprocess.run(argv, cwd=root, env=environment, capture_output=True, timeout=60)
            (output / (name + '.stdout')).write_bytes(result.stdout)
            (output / (name + '.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name, argv=argv, exit_code=result.returncode,
                                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),
                                stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output / 'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
            return result

        for name, source, expected in cases:
            entry = output / (name + '.xr')
            entry.write_text(source, encoding='utf-8')
            result = invoke(name + '-vm', binary, ['run', str(entry)], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, b''), (name, result)
            generated = output / (name + '.c')
            result = invoke(name + '-c', binary, ['build', '--c-only', str(entry), '-o', str(generated)], env)
            assert result.returncode == 0 and not result.stderr, result.stderr
            assert b'({' not in generated.read_bytes()
            native = output / (name + '.exe')
            result = invoke(name + '-build', binary, ['build', str(entry), '-o', str(native)], env)
            assert result.returncode == 0 and not result.stderr, result.stderr
            result = invoke(name + '-native', native, [], hidden)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, b''), (name, result)
        for name, source in [
            ('private', 'import "std/io/output" as Lib\nprint(Lib.write("private",false))\n'),
            ('type', 'import "std/io/output" as Lib\nLib.writeStdout(7)\n'),
            ('private-constant', 'import time as Lib\nprint(Lib.NANOS_PER_MILLI)\n'),
        ]:
            entry = output / (name + '.xr')
            entry.write_text(source, encoding='utf-8')
            result = invoke(name, binary, ['run', str(entry)], hidden)
            assert result.returncode == 1 and not result.stdout and result.stderr, result
        assert hashlib.sha256(poison.read_bytes()).hexdigest() == poison_digest
        # Both absence and invalid source must consume the same Checked closure.
        poison.unlink()
        time_poison.unlink()
        result = invoke('absent-vm', binary, ['run', str(output / 'nominal.xr')], hidden)
        assert (result.returncode, result.stdout, result.stderr) == (0, cases[0][2], b'')
        result = invoke('absent-clock-vm', binary, ['run', str(output / 'clocks.xr')], hidden)
        assert (result.returncode, result.stdout, result.stderr) == (0, cases[2][2], b'')
        (output / 'summary.json').write_text(json.dumps(dict(
            status='CHECKED_OUTPUT_TIME_PRIVATE_CLOSURE_VM_NATIVE_PASS', commands=len(records),
            source_absent_and_poisoned=True, VM_C_provider_PATH_hidden=True,
            new_user_type='User enum in a user generic wrapper', library_generic_templates=False,
            complete_standard_library=False,
            full_resource_and_safety_qualification='OPEN', production_release=False), indent=2)+'\n', encoding='utf-8')
        print('Checked output/time: private closure, user generic values, all clock exports, no-source VM/native and rejection PASS')
        return 0
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
