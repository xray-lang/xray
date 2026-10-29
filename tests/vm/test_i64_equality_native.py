"""Observe both Source equality predicates through an independent native TU."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'compiler', 'include', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='equality-', dir=args.work)).resolve()
    records = []

    def run(name, command):
        result = subprocess.run(list(map(str, command)), cwd=work, capture_output=True, timeout=60)
        (work / (name + '.stdout')).write_bytes(result.stdout)
        (work / (name + '.stderr')).write_bytes(result.stderr)
        records.append({'name': name, 'command': list(map(str, command)), 'exit': result.returncode})
        (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        assert result.returncode == 0 and not result.stderr, (name, work)
        return result.stdout

    observation = json.loads(run('source', [args.driver, '--i64-equality-c', work / 'generated.c']))
    assert observation['sourceLocalBothPredicates'] and observation['sourceWholeBothPredicates']
    callee = observation['callee']
    (work / 'gateway.c').write_text('#include "generated.c"\n'
        'typedef int64_t (*Equality)(int64_t,int64_t);\n'
        f'_Static_assert(_Generic(&xrl_f{callee}, Equality: 1, default: 0), "callee ABI");\n'
        f'Equality equality_get(void) {{ return &xrl_f{callee}; }}\n'
        'int64_t root_run(void) { return xray_lir_entry(); }\n', encoding='ascii')
    (work / 'adapter.c').write_text(r'''#include <stdint.h>
#include <string.h>
typedef int64_t (*Equality)(int64_t,int64_t);
extern Equality equality_get(void);
extern int64_t root_run(void);
int main(void) {
    static const uint64_t vectors[][3] = {
        {0,0,1},{1,2,2},{1,1,1},{0,UINT64_MAX,2},{UINT64_MAX,UINT64_MAX,1},
        {UINT64_C(0x8000000000000000),UINT64_C(0x8000000000000000),1},
        {UINT64_C(0x8000000000000000),INT64_MAX,2},{INT64_MAX,INT64_MAX,1}
    };
    Equality equality=equality_get();
    for(unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);++i) {
        int64_t left,right;memcpy(&left,&vectors[i][0],8);memcpy(&right,&vectors[i][1],8);
        if(equality(left,right)!=(int64_t)vectors[i][2])return 1;
    }
    return root_run()==0?0:2;
}
''', encoding='ascii')
    if args.kind == 'msvc':
        crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
               'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
        flags = ['/nologo', '/std:c11', '/W4', '/WX', '/utf-8', crt, '/O2', '/I' + str(args.include)]
        flags += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
        if args.asan: flags += ['/fsanitize=address', '/clang:-fsanitize=undefined', '/Zi']
        objects = [work / 'gateway.obj', work / 'adapter.obj']
        for source, obj in zip(('gateway.c', 'adapter.c'), objects):
            run('compile-' + source, [args.compiler, *flags, '/c', work / source, '/Fo:' + str(obj)])
        executable = work / 'native.exe'
        run('link', [args.compiler, *flags, *objects, *args.library, '/Fe:' + str(executable),
                     '/link', '/INCREMENTAL:NO'])
    else:
        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', '-fno-lto',
                 '-I' + str(args.include)]
        if args.asan: flags += ['-fsanitize=address,undefined', '-g']
        objects = [work / 'gateway.o', work / 'adapter.o']
        for source, obj in zip(('gateway.c', 'adapter.c'), objects):
            run('compile-' + source, [args.compiler, *flags, '-c', work / source, '-o', obj])
        executable = work / 'native'
        run('link', [args.compiler, *flags, *objects, *args.library, '-lm', '-o', executable])
    assert run('native', [executable]) == b'2\n'
    files = [work / 'generated.c', *objects, executable]
    (work / 'result.json').write_text(json.dumps({'state': 'PASS', 'observation': observation,
        'vectors': 8, 'wholeSourceStdoutHex': '320a',
        'files': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in files]},
        indent=2) + '\n')
    print(work)


if __name__ == '__main__':
    main()
