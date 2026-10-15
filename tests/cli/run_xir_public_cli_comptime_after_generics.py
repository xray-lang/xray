"""Check closed call arguments and open generic scopes before comptime."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from xir_no_child_process import NoChildProcess


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
        temporary = tempfile.TemporaryDirectory(prefix='closed-comptime-', dir=binary.parent)
        output = Path(temporary.name)
    try:
        policy = NoChildProcess(root, output)
        stdlib = output / 'empty-stdlib'
        stdlib.mkdir()
        environment = dict(os.environ, NO_COLOR='1', XRAY_STDLIB_PATH=str(stdlib),
                           PATH=str(output / 'no-C-provider'))
        generic = 'fn echo<T>(value:T)->T{return value}\n'
        packet = 'final class Packet<T>{value:T;init(value){this.value=value}}\n'
        answer = 'fn answer()->i64{return 42}\n'
        folded = 'print(comptime answer())\n'
        positive = [
            ('scalar_only', answer + folded),
            ('generic_declared', generic + answer + folded),
            ('generic_called_before', generic + answer + 'const value=echo<i64>(7)\n' + folded),
            ('generic_called_after', generic + answer + folded + 'const value=echo<i64>(7)\n'),
            ('class_declared', packet + answer + folded),
            ('class_called_before', packet + answer + 'const packet=Packet<i64>(7)\n' + folded),
            ('class_called_after', packet + answer + folded + 'const packet=Packet<i64>(7)\n'),
        ]
        negative = [
            ('open_function', answer + 'fn rejected<T>(value:T)->i64{return comptime answer()}\nprint(1)\n'),
            ('open_class', answer + 'final class Packet<T>{value:T;init(value){this.value=value;const folded=comptime answer()}}\nconst packet=Packet<i64>(7)\n'),
        ]
        records = []
        message = b'comptime in an open generic scope is not supported'
        for rejected, cases in [(False, positive), (True, negative)]:
            for name, source in cases:
                entry = output / (name + '.xr')
                entry.write_text(source, encoding='utf-8')
                argv = [str(binary), 'run', str(entry)]
                result = policy.run(name, argv, environment)
                (output / (name + '.stdout')).write_bytes(result.stdout)
                (output / (name + '.stderr')).write_bytes(result.stderr)
                matched = (result.returncode == 1 and not result.stdout and message in result.stderr
                           if rejected else (result.returncode, result.stdout, result.stderr) == (0, b'42\n', b''))
                records.append(dict(name=name, argv=argv, source_sha256=hashlib.sha256(entry.read_bytes()).hexdigest(),
                                    expected_rejection=rejected, matched=matched, exit_code=result.returncode,
                                    stdout_hex=result.stdout.hex(), stderr=result.stderr.decode('utf-8', errors='replace')))
        identity = subprocess.run([str(binary), '--version', '--json'], cwd=root,
                                  capture_output=True, timeout=30, check=True)
        failed = [row['name'] for row in records if not row['matched']]
        summary = dict(status='FAIL' if failed else 'PASS', cases=records, failed=failed,
                       binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                       version=json.loads(identity.stdout), Windows_absolute_child_denial=policy.control,
                       empty_stdlib=True, C_provider_hidden=True, positive_cases=7, open_scope_rejections=2)
        (output / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(status=summary['status'], positive_cases=7, open_scope_rejections=2,
                              failed=failed)))
        return 1 if failed else 0
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
