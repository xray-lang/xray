"""Execute complete spilled Source graphs through RBC and portable native C."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lir'))
from test_lir_pipeline_eval import MODES, check_receipt, compile_command


def division_matrix():
    source = 'fn q(n:i64,d:i64)->i64{return n/d}\nfn r(n:i64,d:i64)->i64{return n%d}\n'
    expected = b''
    for n in [-9223372036854775808, -98, -7, -1, 0, 1, 7, 98, 9223372036854775807]:
        for d in [-97, -3, -1, 1, 2, 3, 97]:
            operand = '-9223372036854775807-1' if n == -9223372036854775808 else str(n)
            source += f'print(q({operand},{d}))\nprint(r({operand},{d}))\n'
            q = abs(n) // abs(d) * (-1 if (n < 0) != (d < 0) else 1)
            r = n - q * d
            if q == 9223372036854775808:
                q = -9223372036854775808
            expected += f'{q}\n{r}\n'.encode()
    source = source.encode()
    assert len(source) == 2540 and len(expected) == 550
    assert hashlib.sha256(source).hexdigest() == '5b0ebf841431da39e8996d42c0e7bbb2ea70b1054f6285e5e1fddc289490e63e'
    assert hashlib.sha256(expected).hexdigest() == '6d3a94fd9bee1bdefac61f87ce9dc5a447e9c58f1ba571432f716c704793c6c3'
    return source, expected


def pressure_loop(text):
    # Preserve all 130 dynamic terms without exercising the independent
    # Source discovery recursion limit with a left-deep expression.
    terms = [f'(i+{k})' for k in range(130)]
    while len(terms) > 1:
        terms = ['(' + terms[k] + '+' + terms[k+1] + ')' if k+1 < len(terms) else terms[k]
                 for k in range(0, len(terms), 2)]
    source = 'fn pressure(n:i64)->i64{var i=0\nvar s=0\nwhile(i<n){\n'
    if text:
        source += 'const temporary="n=${i}"\nprint(temporary)\n'
    source += f's=s+{terms[0]}\ni=i+1\n}}\nreturn s\n}}\n'
    expected = b''
    for count in [0, 1, 5]:
        source += f'print(pressure({count}))\n'
        if text:
            expected += b''.join(f'n={i}\n'.encode() for i in range(count))
        expected += f'{sum(sum(i+k for k in range(130)) for i in range(count))}\n'.encode()
    return source.encode(), expected


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'compiler', 'include', 'source-root', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='spill-cfg-', dir=args.work)).resolve()
    rows = []

    def run(folder, label, command, expected=None):
        command = list(map(str, command))
        (folder / (label + '.command.json')).write_text(json.dumps(command, indent=2) + '\n', encoding='utf-8')
        result = subprocess.run(command, cwd=folder, capture_output=True, timeout=60)
        for suffix, data in [('stdout', result.stdout), ('stderr', result.stderr)]:
            (folder / (label + '.' + suffix)).write_bytes(data)
        (folder / (label + '.exit')).write_text(str(result.returncode), encoding='ascii')
        assert result.returncode == 0 and not result.stderr, (folder, label)
        if expected is not None:
            assert result.stdout == expected, (folder, label, len(result.stdout))

    cases = [('complete-divrem', division_matrix()), ('scalar-loop', pressure_loop(False)),
             ('temporary-string-loop', pressure_loop(True))]
    for name, (program, expected) in cases:
        source = root / (name + '.xr')
        source.write_bytes(program)
        for mode in MODES:
            folder = root / (name + '-' + mode)
            folder.mkdir()
            generated = folder / 'generated.c'
            receipt_path = folder / 'receipt.json'
            run(folder, 'producer', [args.driver, source, mode, generated, receipt_path, '0', '2', 'normal'], expected * 2)
            receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
            check_receipt(receipt, mode, 2)
            if name != 'complete-divrem' or mode in ('inline', 'sccp-inline-inline-sccp'):
                assert receipt['rbc_spills'] > 0, (name, mode, receipt['rbc_spills'])
            assert '({' not in generated.read_text(encoding='ascii')
            client = folder / 'client.c'
            client.write_text('#include <stdint.h>\n#include "generated.c"\n'
                '_Static_assert(_Generic(&xray_lir_entry,int64_t(*)(void):1,default:0),"root ABI");\n'
                'int main(void){if(xray_lir_entry()!=0)return 1;return xray_lir_entry()!=0;}\n', encoding='ascii')
            executable = folder / ('client.exe' if args.kind == 'msvc' else 'client')
            run(folder, 'compile', compile_command(args, client, executable))
            run(folder, 'native', [executable], expected * 2)
            rows.append({'case': name, 'mode': mode, 'sourceBytes': len(program),
                         'sourceSHA256': hashlib.sha256(program).hexdigest(),
                         'oracleBytes': len(expected), 'oracleSHA256': hashlib.sha256(expected).hexdigest(),
                         'rbcSpills': receipt['rbc_spills'], 'rbcReferenceSpills': receipt['rbc_reference_spills'],
                         'RBCEntries': 2, 'nativeEntries': 2, 'state': 'PASS'})
            assert source.read_bytes() == program
    (root / 'result.json').write_text(json.dumps({'state': 'PASS', 'rows': rows}, indent=2) + '\n', encoding='utf-8')
    print('Complete typed-spill CFG checks PASS: ' + str(root), flush=True)


if __name__ == '__main__':
    main()
