"""Consume the actual Source negation callee through a separate native ABI TU."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lir'))
from test_lir_carriers_native import _f64_profile, _f64_reject


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'compiler', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='fneg-', dir=args.work)).resolve()
    records = []

    def supported(flags, command, product):
        profile = _f64_profile(args.compiler,args.kind,flags,work)
        if profile['supported']:
            return True
        rejected = _f64_reject(command,work,profile['expectedGuard'],product)
        (work / 'result.json').write_text(json.dumps({'state': 'UNSUPPORTED_TARGET_REJECTED',
            'profile': profile, 'rejection': rejected, 'payloadVectorsExecuted': 0},indent=2) + '\n')
        print(work)
        return False

    def run(name, command):
        result = subprocess.run(list(map(str, command)), cwd=work, capture_output=True, timeout=60)
        (work / (name + '.stdout')).write_bytes(result.stdout)
        (work / (name + '.stderr')).write_bytes(result.stderr)
        records.append({'name': name, 'command': list(map(str, command)), 'exit': result.returncode})
        (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        assert result.returncode == 0 and not result.stderr, (name, work)
        return result.stdout

    observation = json.loads(run('source', [args.driver, '--fneg-c', work / 'generated.c']))
    assert observation['sourceLocalFneg'] and observation['sourceWholeFneg']
    callee = observation['callee']
    (work / 'gateway.c').write_text('#include "generated.c"\n'
        'typedef double (*Negate)(double);\n'
        f'_Static_assert(_Generic(&xrl_f{callee}, Negate: 1, default: 0), "callee ABI");\n'
        f'Negate negate_get(void) {{ return &xrl_f{callee}; }}\n', encoding='ascii')
    vectors = Path(__file__).with_name('f64_negate_vectors.def').read_text(encoding='ascii')
    rows = re.findall(r'XR_FNEG\(([0-9a-f]{16}), ([0-9a-f]{16})\)', vectors)
    assert len(rows) == 21
    (work / 'vectors.h').write_text('\n'.join(
        '{UINT64_C(0x' + source + '),UINT64_C(0x' + expected + ')},' for source, expected in rows),
        encoding='ascii')
    (work / 'adapter.c').write_text(r'''#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <fenv.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define CONTROL_CASES 4u
#else
#define CONTROL_CASES 1u
#endif
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif
typedef double (*Negate)(double);
extern Negate negate_get(void);
int main(void) {
    static const uint64_t vectors[][2] = {
#include "vectors.h"
    };
    const int modes[] = {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    fenv_t original; if (fegetenv(&original)) return 1;
#if CONTROL_CASES == 4u
    unsigned original_control = _mm_getcsr();
#endif
    Negate negate = negate_get();
    for (unsigned m=0;m<4;++m) for(unsigned c=0;c<CONTROL_CASES;++c) {
        if(fesetround(modes[m]) || feclearexcept(FE_ALL_EXCEPT) || feraiseexcept(FE_INEXACT)) return 2;
#if CONTROL_CASES == 4u
        unsigned mask=(1u<<6)|(1u<<15);
        _mm_setcsr((_mm_getcsr() & ~mask) | ((c&1u)<<6) | (((c>>1)&1u)<<15));
        unsigned control=_mm_getcsr();
#endif
        int flags=fetestexcept(FE_ALL_EXCEPT);
        for(unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);++i) {
            double input; memcpy(&input,&vectors[i][0],8);
            double answer=negate(input);
            int after_flags=fetestexcept(FE_ALL_EXCEPT),after_round=fegetround();
#if CONTROL_CASES == 4u
            unsigned after_control=_mm_getcsr();
#endif
            uint64_t bits; memcpy(&bits,&answer,8);
            if(bits!=vectors[i][1] || after_flags!=flags || after_round!=modes[m]) return 3;
#if CONTROL_CASES == 4u
            if(after_control!=control) return 4;
#endif
        }
    }
    if(fesetenv(&original)) return 5;
#if CONTROL_CASES == 4u
    _mm_setcsr(original_control);
#endif
    puts("Source F64 negation ABI and fenv passed"); return 0;
}
''', encoding='ascii')
    if args.kind == 'msvc':
        crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
               'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
        flags = ['/nologo', '/std:c11', '/W4', '/WX', '/utf-8', crt, '/O2']
        flags += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
        if args.asan: flags += ['/fsanitize=address', '/clang:-fsanitize=undefined', '/Zi']
        objects = [work / 'gateway.obj', work / 'adapter.obj']
        if not supported(flags,[args.compiler,*flags,'/c',work/'gateway.c','/Fo:'+str(objects[0])],objects[0]):
            return
        for source, obj in zip(('gateway.c', 'adapter.c'), objects):
            run('compile-' + source, [args.compiler, *flags, '/c', work / source, '/Fo:' + str(obj)])
        executable = work / 'native.exe'
        run('link', [args.compiler, *flags, *objects, '/Fe:' + str(executable), '/link', '/INCREMENTAL:NO'])
    else:
        flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', '-fno-lto']
        if args.asan: flags += ['-fsanitize=address,undefined', '-g']
        objects = [work / 'gateway.o', work / 'adapter.o']
        if not supported(flags,[args.compiler,*flags,'-c',work/'gateway.c','-o',objects[0]],objects[0]):
            return
        for source, obj in zip(('gateway.c', 'adapter.c'), objects):
            run('compile-' + source, [args.compiler, *flags, '-c', work / source, '-o', obj])
        executable = work / 'native'
        run('link', [args.compiler, *flags, *objects, '-lm', '-o', executable])
    assert run('native', [executable]).strip() == b'Source F64 negation ABI and fenv passed'
    files = [work / 'generated.c', *objects, executable]
    (work / 'result.json').write_text(json.dumps({'state': 'PASS', 'observation': observation,
        'vectors': len(rows), 'roundingModes': 4, 'controlCases': 4 if args.kind == 'msvc' else None,
        'files': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in files]},
        indent=2) + '\n')
    print(work)


if __name__ == '__main__':
    main()
