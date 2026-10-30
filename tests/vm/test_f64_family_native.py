"""Run complete Source float witnesses and cold public build-closure clients."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lir'))
from test_lir_carriers_native import _f64_profile, _f64_reject


def adapter(mode, cpp=False):
    public = mode == 'public'
    text = '#include <stdint.h>\n#include <string.h>\n#include <fenv.h>\n'
    if public:
        text += '#include "xray_native_runtime.h"\n'
        if cpp:
            text += ('#include <type_traits>\n'
                '#define ABI(f,t) static_assert(std::is_same<decltype(&f),t>::value,"public ABI")\n')
        else:
            text += '#define ABI(f,t) _Static_assert(_Generic(&f,t:1,default:0),"public ABI")\n'
        text += ('typedef uint64_t (*Binary)(uint64_t,uint64_t);\n'
            'typedef int32_t (*Compare)(uint64_t,uint64_t);\n'
            'ABI(xrt_f64_add,Binary);ABI(xrt_f64_subtract,Binary);\n'
            'ABI(xrt_f64_multiply,Binary);ABI(xrt_f64_divide,Binary);ABI(xrt_f64_compare,Compare);\n')
    elif mode == 'arithmetic':
        text += 'typedef double (*Arithmetic)(int64_t,double,double);\nextern Arithmetic arithmetic_get(void);\n'
    else:
        text += 'typedef int64_t (*Relations)(double,double);\nextern Relations relations_get(void);\n'
    if not public:
        text += 'extern int64_t root_run(void);\n'
    text += r'''#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define CONTROLS 4u
#else
#define CONTROLS 1u
#endif
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif
'''
    if mode != 'relations':
        text += ('static const struct {uint32_t op;uint64_t left,right,expected;} vectors[]={\n'
                 '#include "arithmetic-values.h"\n};\n')
    if mode != 'arithmetic':
        text += ('static const struct {uint64_t bits;int rank;} orders[]={\n'
                 '#include "order-values.h"\n};\n')
    text += r'''int main(void) {
    const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    fenv_t original;if(fegetenv(&original))return 1;
#if CONTROLS == 4u
    unsigned original_control=_mm_getcsr();
#endif
'''
    if public:
        text += '    Binary binary[]={xrt_f64_add,xrt_f64_subtract,xrt_f64_multiply,xrt_f64_divide};\n'
    elif mode == 'arithmetic':
        text += '    Arithmetic arithmetic=arithmetic_get();\n'
    else:
        text += '    Relations relations=relations_get();\n'
    text += r'''    for(unsigned m=0;m<4;++m)for(unsigned c=0;c<CONTROLS;++c) {
        if(fesetround(modes[m]) || feclearexcept(FE_ALL_EXCEPT) || feraiseexcept(FE_INEXACT))return 2;
#if CONTROLS == 4u
        unsigned mask=(1u<<6)|(1u<<15);
        _mm_setcsr((_mm_getcsr()&~mask)|((c&1u)<<6)|(((c>>1)&1u)<<15));
        unsigned control=_mm_getcsr();
#define ENV_OK() (fetestexcept(FE_ALL_EXCEPT)==flags && fegetround()==modes[m] && _mm_getcsr()==control)
#else
#define ENV_OK() (fetestexcept(FE_ALL_EXCEPT)==flags && fegetround()==modes[m])
#endif
        int flags=fetestexcept(FE_ALL_EXCEPT);
'''
    if mode != 'relations':
        text += '        unsigned count=0;\n        for(unsigned i=0;i<sizeof(vectors)/sizeof(*vectors);++i) {\n'
        if public:
            text += ('            uint64_t bits=binary[vectors[i].op](vectors[i].left,vectors[i].right);\n'
                     '            if(!ENV_OK())return 3;\n')
        else:
            text += ('            double left,right;memcpy(&left,&vectors[i].left,8);memcpy(&right,&vectors[i].right,8);\n'
                     '            double answer=arithmetic((int64_t)vectors[i].op,left,right);\n'
                     '            if(!ENV_OK())return 3;\n'
                     '            uint64_t bits;memcpy(&bits,&answer,8);\n')
        text += ('            if(bits!=vectors[i].expected)return 4;\n            ++count;\n'
                 '        }\n        if(count!=3216)return 5;\n')
    if mode != 'arithmetic':
        text += '        for(unsigned a=0;a<26;++a)for(unsigned b=0;b<26;++b) {\n'
        if public:
            text += ('            int32_t value=xrt_f64_compare(orders[a].bits,orders[b].bits);\n'
                     '            if(!ENV_OK())return 6;\n'
                     '            int x=orders[a].rank,y=orders[b].rank;\n'
                     '            int32_t expected=x<0 || y<0?2:x==y?0:x<y?-1:1;\n')
        else:
            text += ('            double left,right;memcpy(&left,&orders[a].bits,8);memcpy(&right,&orders[b].bits,8);\n'
                     '            int64_t value=relations(left,right);\n            if(!ENV_OK())return 6;\n'
                     '            int x=orders[a].rank,y=orders[b].rank;\n'
                     '            int64_t expected=x<0 || y<0?2:x==y?41:x<y?14:50;\n')
        text += '            if(value!=expected)return 7;\n        }\n'
    text += r'''    }
    if(fesetenv(&original))return 8;
#if CONTROLS == 4u
    _mm_setcsr(original_control);
#endif
'''
    text += '    return 0;\n}\n' if public else '    return root_run()==0?0:9;\n}\n'
    return text


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'compiler', 'cxx-compiler', 'include', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='family-', dir=args.work)).resolve()
    commands = []

    def run(name, command, folder=work):
        result = subprocess.run(list(map(str, command)), cwd=folder, capture_output=True, timeout=60)
        (folder / (name + '.stdout')).write_bytes(result.stdout)
        (folder / (name + '.stderr')).write_bytes(result.stderr)
        commands.append({'name': name, 'directory': str(folder), 'command': list(map(str, command)),
                         'exit': result.returncode})
        (work / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        assert result.returncode == 0 and not result.stderr, (name, folder)
        return result.stdout

    observation = json.loads(run('source', [args.driver, '--f64-family-c', work]))
    assert observation['arithmeticOps'] == 4 and observation['relationPredicates'] == 6
    assert run('rbc-whole', [args.driver, '--f64-family-rbc']) == b'14\n'
    golden = Path(__file__).resolve().parents[1] / 'unit/xir/xir_float_arithmetic_vectors.def'
    assert hashlib.sha256(golden.read_bytes().replace(b'\r\n', b'\n')).hexdigest() == 'f55ebc6c5529d16793607901910a6707d65b2df8a311b1820a30b218ee52805c'
    rows = re.findall(r'^XIR_FLOAT_ARITHMETIC\((32|64), XR_XIR_FLOAT_(\w+), '
        r'UINT64_C\(0x([0-9a-f]{16})\), UINT64_C\(0x([0-9a-f]{16})\), UINT64_C\(0x([0-9a-f]{16})\)\)',
        golden.read_text(), re.M)
    assert len(rows) == 6432
    rows = [x for x in rows if x[0] == '64'];assert len(rows) == 3216
    operations = {'ADD': 0, 'SUBTRACT': 1, 'MULTIPLY': 2, 'DIVIDE': 3}
    arithmetic = '\n'.join('{' + str(operations[op]) + ',' + ','.join(
        'UINT64_C(0x' + value + ')' for value in (left, right, expected)) + '},'
        for _, op, left, right, expected in rows)
    values = re.findall(r'XR_F64_VALUE\(([0-9a-f]{16}), (-?\d+)\)',
        Path(__file__).with_name('f64_compare_values.def').read_text());assert len(values) == 26
    order = '\n'.join('{UINT64_C(0x' + bits + '),' + rank + '},' for bits, rank in values)

    def flags(cpp=False):
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            result = ['/nologo', '/std:c++17' if cpp else '/std:c11', '/W4', '/WX', '/utf-8', crt, '/O2']
            if cpp: result += ['/EHsc']
            result += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
            if args.asan: result += ['/fsanitize=address', '/clang:-fsanitize=undefined', '/Zi']
            return result
        return ['-std=c++17' if cpp else '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
                '-fno-lto', *(['-fsanitize=address,undefined', '-g'] if args.asan else [])]

    def compile_command(compiler, options, src, obj):
        return [compiler, *options, '/c', src, '/Fo:' + str(obj)] if args.kind == 'msvc' else [
            compiler, *options, '-c', src, '-o', obj]

    base = flags();profile = _f64_profile(args.compiler, args.kind, base, work)
    if not profile['supported']:
        obj = work / 'rejected.obj'
        rejected = _f64_reject(compile_command(args.compiler, base, work / 'arithmetic.c', obj),
                               work, profile['expectedGuard'], obj)
        (work / 'result.json').write_text(json.dumps({'state': 'UNSUPPORTED_TARGET_REJECTED',
            'profile': profile, 'rejection': rejected, 'nativePayloadVectorsExecuted': 0}, indent=2) + '\n')
        print(work);return
    artifacts = []

    def build_run(folder, sources, include, libraries, cpp=False, expected=b''):
        options = flags(cpp) + [('/I' if args.kind == 'msvc' else '-I') + str(include)]
        compiler = args.cxx_compiler if cpp else args.compiler
        objects = [folder / (p.stem + ('.obj' if args.kind == 'msvc' else '.o')) for p in sources]
        for src, obj in zip(sources, objects):
            run('compile-' + src.name, compile_command(compiler, options, src, obj), folder)
        executable = folder / ('native.exe' if args.kind == 'msvc' else 'native')
        command = [compiler, *[option for option in options if option != '/EHsc'], *objects, *libraries]
        command += ['/Fe:' + str(executable), '/link', '/INCREMENTAL:NO'] if args.kind == 'msvc' else ['-lm', '-o', executable]
        run('link', command, folder);assert run('native', [executable], folder) == expected
        artifacts.append({'mode': folder.name, 'cpp': cpp, 'arithmeticRows': 0 if folder.name == 'relations' else 3216,
            'comparisonPairs': 0 if folder.name == 'arithmetic' else 676, 'roundingModes': 4,
            'controlCases': 4, 'files': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                for p in [*sources, *objects, executable]]})

    for mode in ('arithmetic', 'relations'):
        folder = work / mode;folder.mkdir()
        shutil.copy2(work / (mode + '.c'), folder / 'generated.c')
        (folder / 'arithmetic-values.h').write_text(arithmetic, encoding='ascii')
        (folder / 'order-values.h').write_text(order, encoding='ascii')
        callee = observation['arithmeticCallee' if mode == 'arithmetic' else 'relationsCallee']
        signature = 'double (*Callable)(int64_t,double,double)' if mode == 'arithmetic' else 'int64_t (*Callable)(double,double)'
        (folder / 'gateway.c').write_text('#include "generated.c"\ntypedef ' + signature + ';\n'
            f'_Static_assert(_Generic(&xrl_f{callee}, Callable:1,default:0),"Source ABI");\n'
            f'Callable {mode}_get(void) {{ return &xrl_f{callee}; }}\n'
            'int64_t root_run(void) { return xray_lir_entry(); }\n', encoding='ascii')
        (folder / 'adapter.c').write_text(adapter(mode), encoding='ascii')
        build_run(folder, [folder / 'gateway.c', folder / 'adapter.c'], args.include, args.library,
                  expected=b'14\n' if mode == 'relations' else b'')
    closure = work / 'moved-build-closure';(closure / 'include').mkdir(parents=True);(closure / 'lib').mkdir()
    copied = []
    for path in [args.include / 'xray_native_runtime.h', args.include / 'xray_export.h', *args.library]:
        target = closure / ('include' if path.suffix == '.h' else 'lib') / path.name
        shutil.copy2(path, target);assert path.read_bytes() == target.read_bytes()
        copied.append({'source': str(path), 'path': str(target), 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
    assert len(args.library) == 5
    libraries = [closure / 'lib' / p.name for p in args.library]
    for cpp in (False, True):
        folder = closure / ('public-cpp17' if cpp else 'public-c11');folder.mkdir()
        (folder / 'arithmetic-values.h').write_text(arithmetic, encoding='ascii')
        (folder / 'order-values.h').write_text(order, encoding='ascii')
        src = folder / ('consumer.cpp' if cpp else 'consumer.c');src.write_text(adapter('public', cpp), encoding='ascii')
        build_run(folder, [src], closure / 'include', libraries, cpp)
    negative = []
    clean = [x for x in base if x != '/Zi' and not x.startswith(('/fsanitize=', '/clang:-fsanitize=', '-fsanitize='))]
    controls = [('fast', ['/fp:fast'] if args.kind == 'msvc' else ['-ffast-math'])]
    if args.kind == 'msvc': controls.append(('abi', ['/D_M_ARM64EC=1']))
    for label, options in controls:
        folder = work / ('negative-' + label);folder.mkdir()
        options = [*clean, *options, ('/I' if args.kind == 'msvc' else '-I') + str(args.include)]
        facts = _f64_profile(args.compiler, args.kind, options, folder);assert not facts['supported']
        obj = folder / 'rejected.obj'
        rejection = _f64_reject(compile_command(args.compiler, options, work / 'arithmetic.c', obj),
                               folder, facts['expectedGuard'], obj)
        negative.append({'control': label, 'facts': facts, 'rejection': rejection})
    (work / 'result.json').write_text(json.dumps({'state': 'PASS', 'observation': observation,
        'profile': profile, 'allKernelGoldenRowsPreserved': 6432, 'F64RowsActuallyExecuted': 3216,
        'comparisonPairsActuallyExecuted': 676, 'nativeProducts': artifacts, 'movedBuildClosure': copied,
        'distribution': 'Build closure moved for cold public C11/C++17 consumers; no cmake installation or official SDK claim.',
        'negativeControls': negative}, indent=2) + '\n')
    print(work)


if __name__ == '__main__':
    main()
