"""Execute whole Source and independent LIR division through RBC and real C11."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import tempfile
from test_lir_pipeline_eval import compile_command


ORIGINAL_CASES = {'family': (b'fn quotient(n:i64,d:i64)->i64{return n/d}\nfn remainder(n:i64,d:i64)->i64{return n%d}'
            b'\nprint(quotient(8,2))\nprint(remainder(-7,3))\nprint(quotient(-9223372036854775807'
            b'-1,-1))\nprint(remainder(-9223372036854775807-1,-1))\nprint(quotient(7,-3))\nprint('
            b'remainder(7,-3))\n',
            b'4\n-1\n-9223372036854775808\n0\n-2\n1\n',
            0),
 'order': (b'fn left(n:i64)->i64{print("left")\nreturn n}\nfn right(d:i64)->i64{print("right")\n'
           b'return d}\nprint(left(7)/right(3))\nprint(left(-7)%right(3))\n',
           b'left\nright\n2\nleft\nright\n-1\n',
           0),
 'division-zero': (b'fn quotient(n:i64,d:i64)->i64{return n/d}\nprint(1)\nprint(quotient(7,0))\n'
                   b'print(99)\n',
                   b'1\n',
                   420),
 'remainder-zero': (b'fn remainder(n:i64,d:i64)->i64{return n%d}\nprint(1)\nprint(remainder(7,0)'
                    b')\nprint(99)\n',
                    b'1\n',
                    421),
 'original-whole': (b'print(1)\nfn value(n:i64,d:i64)->i64{return n/d}\nvalue(8,2)\n', b'1\n', 0)}


def invoke(root, name, command, status=0, output=None, diagnostic=None):
    command = [str(value) for value in command]
    p = subprocess.run(command, cwd=root, capture_output=True, timeout=60)
    for suffix, data in [('stdout', p.stdout), ('stderr', p.stderr)]:
        (root / (name + '.' + suffix)).write_bytes(data)
    record = {'command': command, 'exit': p.returncode,
              'stdoutSHA256': hashlib.sha256(p.stdout).hexdigest(),
              'stderrSHA256': hashlib.sha256(p.stderr).hexdigest()}
    (root / (name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
    assert p.returncode == status, (name, p.returncode, root)
    if output is not None:
        assert p.stdout == output, (name, p.stdout, output, root)
    if diagnostic is not None:
        assert p.stderr == diagnostic, (name, p.stderr, diagnostic, root)
    return p


def adapter(generated, result, panic, location):
    text = '#define _CRT_SECURE_NO_WARNINGS 1\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
    if panic:
        text += '#include "runtime/native/xnative_execution.h"\n'
    text += '#include "' + generated.name + '"\n'
    text += '_Static_assert(_Generic(&xray_lir_entry,int64_t(*)(void):1,default:0),"complete root");\n'
    if panic:
        line, column, end_line, end_column, path = location
        text += 'static void on_exit(void){fputs("EXIT_CALLBACK",stderr);}\n'
        text += 'int main(int argc,char **argv){(void)argv;\n'
        text += 'if(argc>1){XrtLocationOwner *owner=NULL;XrtExecutionFailure failure={0};int64_t value=0;\n'
        text += 'if(xrt_locations_create(xrt_standalone_source_map.locations,xrt_standalone_source_map.count,&owner)!=XRT_LOCATION_OK)return 2;\n'
        text += 'if(xrt_run_embedded(owner,&failure,xrl_invoke,&value)!=XRT_EXECUTION_FAILURE)return 3;\n'
        text += 'if(failure.kind!=XRT_FAILURE_ARITHMETIC_PANIC||failure.detail.panic.code!=' + str(panic) + ')return 4;\n'
        text += 'const XrtSourceLocation *point=xrt_locations_get(owner,failure.detail.panic.source_id);\n'
        if path is None:
            text += 'if(point||failure.detail.panic.source_id!=0)return 5;\n'
        else:
            escaped = json.dumps(path)
            text += 'if(!point||point->line!=' + str(line) + '||point->column!=' + str(column) + '||point->end_line!=' + str(end_line) + '||point->end_column!=' + str(end_column) + ')return 6;\n'
            text += 'if(point->path_length!=sizeof(' + escaped + ')-1||memcmp(point->path,' + escaped + ',point->path_length))return 7;\n'
        text += 'xrt_locations_release(owner);return 0;}\n'
        text += 'if(atexit(on_exit))return 8;(void)xray_lir_entry();fputs("AFTER_ENTRY",stderr);return 9;}\n'
    else:
        text += 'int main(void){return xray_lir_entry()==(' + str(result) + ')?0:1;}\n'
    return text


def main():
    p = argparse.ArgumentParser()
    for name in ['driver', 'fixture-driver', 'vm-driver', 'compiler', 'include', 'source-root', 'work']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--library', type=Path, nargs='+', required=True)
    p.add_argument('--kind', choices=['msvc', 'gnu'], required=True)
    p.add_argument('--msvc-runtime', default='MultiThreaded')
    p.add_argument('--asan', action='store_true')
    args = p.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='divrem-', dir=args.work))
    cases = dict(ORIGINAL_CASES)
    program = 'fn q(n:i64,d:i64)->i64{return n/d}\nfn r(n:i64,d:i64)->i64{return n%d}\n'
    definitions = program
    small_programs = []
    expected = b''
    for n in [-9223372036854775808, -98, -7, -1, 0, 1, 7, 98, 9223372036854775807]:
        for d in [-97, -3, -1, 1, 2, 3, 97]:
            source_n = '-9223372036854775807-1' if n == -9223372036854775808 else str(n)
            calls = f'print(q({source_n},{d}))\nprint(r({source_n},{d}))\n'
            program += calls
            q = abs(n) // abs(d) * (-1 if (n < 0) != (d < 0) else 1)
            r = n - q * d
            if q == 9223372036854775808:
                q = -9223372036854775808
            observed = f'{q}\n{r}\n'.encode()
            expected += observed
            small_programs.append(((definitions + calls).encode(), observed))
    cases['signs-extremes'] = (program.encode(), expected, 0)
    cases['computed'] = (b'fn q(n:i64,d:i64)->i64{return n/(d+2)}\nfn r(n:i64,flag:bool)->i64{return n%(flag?3:7)}\nprint(q(8,1))\nprint(r(8,true))\nprint(r(8,false))\n', b'2\n2\n1\n', 0)
    cases['direct-zero'] = (b'fn q(n:i64)->i64{return n/0}\nprint(1)\nprint(q(8))\nprint(99)\n', b'1\n', 420)
    cases['module-zero'] = (b'const divisor=0\nfn r(n:i64)->i64{return n%divisor}\nprint(1)\nprint(r(8))\nprint(99)\n', b'1\n', 421)
    rows = []
    for name, (program, expected, panic) in cases.items():
        source = root / (name + '.xr')
        source.write_bytes(program)
        # Source lowering retains the return statement's existing debug point.
        origin = program.find(b'return ') if panic else -1
        line = program[:origin].count(b'\n') + 1
        column = origin - program.rfind(b'\n', 0, origin)
        point = (line, column, line, column, str(source))
        for suffix, options in [('raw', []), ('optimized', ['--optimize'])]:
            label = name + '-' + suffix
            diagnostic = (f'[Uncaught Panic] E0{panic}: ' + ('division' if panic == 420 else 'modulo') + ' by zero\n').encode() if panic else b''
            invoke(root, label + '-rbc', [args.vm_driver, source, *options], int(bool(panic)), expected, diagnostic)
            generated = root / (label + '.c')
            invoke(root, label + '-emit', [args.driver, source, generated, *options], output=b'', diagnostic=b'')
            text = generated.read_text(encoding='ascii')
            assert '({' not in text
            client = root / (label + '-client.c')
            client.write_text(adapter(generated, 0, panic, point), encoding='ascii')
            executable = root / (label + '.exe')
            invoke(root, label + '-compile', compile_command(args, client, executable))
            invoke(root, label + '-native', [executable], int(bool(panic)), expected, diagnostic)
            if panic:
                invoke(root, label + '-embedded', [executable, 'embedded'], output=expected, diagnostic=b'')
            rows.append({'case': label, 'sourceSHA256': hashlib.sha256(program).hexdigest(), 'panic': panic,
                         'rbc': 'PASS'})
        assert source.read_bytes() == program
    # Keep the independently expected pairs in addition to the complete matrix.
    for i, (program, expected) in enumerate(small_programs):
        source = root / f'pair-{i}.xr'
        source.write_bytes(program)
        invoke(root, f'pair-{i}-optimized-rbc', [args.vm_driver, source, '--optimize'], output=expected, diagnostic=b'')
    for kind in [0, 1, 2, 3, 4, 6, 7]:
        for opcode in range(2):
            label = f'division-{kind}-{opcode}'
            generated = root / (label + '.c')
            invoke(root, label + '-emit', [args.fixture_driver, label, generated], output=b'', diagnostic=b'')
            panic = 420 + opcode if kind in [0, 7] else 0
            answer = {1: (-17, 0), 2: (1, 0), 3: (2, 5), 4: (5, 2), 6: (5, 2)}.get(kind, (0, 0))[opcode]
            point = (7, 3, 8, 9, 'arithmetic-owned.xr') if kind == 7 else (0, 0, 0, 0, None)
            client = root / (label + '-client.c')
            client.write_text(adapter(generated, answer, panic, point), encoding='ascii')
            executable = root / (label + '.exe')
            invoke(root, label + '-compile', compile_command(args, client, executable))
            diagnostic = (f'[Uncaught Panic] E0{panic}: ' + ('division' if opcode == 0 else 'modulo') + ' by zero\n').encode() if panic else b''
            invoke(root, label + '-native', [executable], int(bool(panic)), b'', diagnostic)
            if panic:
                invoke(root, label + '-embedded', [executable, 'embedded'], output=b'', diagnostic=b'')
            rows.append({'case': label, 'panic': panic})
    result = {'state': 'PASS', 'rows': rows, 'nativeImages': len(rows),
              'smallOptimizedRbcPrograms': len(small_programs),
              'largeOptimizedRbc': 'PASS'}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Dynamic division regression checks PASS, including complete optimized RBC matrix: ' + str(root), flush=True)


if __name__ == '__main__':
    main()
