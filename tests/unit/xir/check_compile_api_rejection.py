"""Reject old or incorrectly owned function prototypes without linking."""
from pathlib import Path
import argparse
import json
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', required=True)
parser.add_argument('--msvc', choices=('0', '1'), required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
cases = {
    'check_without_context': ('xr_xir_compile_check', 'XrXirStatus',
        'const XrXirModule *, XrXirArtifact **, XrXirDiagnostic *'),
    'seal_with_value_budget': ('xr_xir_compile_program_seal', 'XrXirStatus',
        'const XrXirProgramSpec *, OldBudget, XrXirProgram **'),
    'take_with_fresh_context': ('xr_xir_compile_vm_program_take', 'XrXirStatus',
        'const XrXirCompileContext *, XrXirArtifact **, XrXirProgram **'),
    'emit_with_mutable_owner': ('xr_xir_compile_emit_c', 'XrXirStatus',
        'XrXirArtifact *, const char *, size_t, XrXirCSource *'),
}
records = []
for name, (symbol, result, parameters) in cases.items():
    source = args.output / (name + '.c')
    obj = args.output / (name + '.obj')
    source.write_text('#include "xir/xxir_vm.h"\n#include "xir/xxir_emit_c.h"\n'
        'typedef struct OldBudget { uint64_t metadata_bytes, work; } OldBudget;\n'
        f'typedef {result} (*Incorrect)({parameters});\n'
        f'_Static_assert(_Generic(&{symbol}, Incorrect: 1, default: 0), "XR_NEGATIVE_SIGNATURE");\n',
        encoding='utf-8')
    if args.msvc == '1':
        command = [args.compiler, '/nologo', '/std:c11', '/utf-8', '/TC', '/c',
            '/I' + str(args.root / 'src'), '/Fo' + str(obj), str(source)]
    else:
        command = [args.compiler, '-std=c11', '-I' + str(args.root / 'src'),
            '-c', str(source), '-o', str(obj)]
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    (args.output / (name + '.log')).write_bytes(process.stdout)
    passed = process.returncode != 0 and b'XR_NEGATIVE_SIGNATURE' in process.stdout
    records.append(dict(name=name, command=command, exit=process.returncode, rejected=passed))
    if not passed:
        raise SystemExit(f'Incorrect signature did not fail its explicit assertion: {name}')
(args.output / 'record.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('Four independent incorrect prototypes rejected by C11 static assertions')
