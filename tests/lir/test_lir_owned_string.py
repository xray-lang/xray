"""Compare complete Source String programs through owned LIR, RBC and native C."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'source-driver', 'result-driver', 'compiler', 'include', 'source-root', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='owned-string-', dir=args.work)).resolve()
    records = []

    def run(label, command, directory=root, stdout=subprocess.PIPE):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=directory, stdout=stdout, stderr=subprocess.PIPE, timeout=60)
        (directory / (label + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        (directory / (label + '.stdout')).write_bytes(result.stdout or b'')
        (directory / (label + '.stderr')).write_bytes(result.stderr)
        (directory / (label + '.exit')).write_text(str(result.returncode))
        return result

    def compile_run(folder, generated, main, optimization, expected):
        executable = folder / ('native-' + optimization + ('.exe' if args.kind == 'msvc' else ''))
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            flags = ['/nologo', '/std:c11', '/W4', '/WX', '/utf-8', crt,
                     '/' + optimization, '/I' + str(args.include)]
            if args.asan:
                flags += ['/fsanitize=address', '/clang:-fsanitize=undefined']
        else:
            flag = {'Od': '-O0', 'O1': '-O1', 'O2': '-O2', 'Ox': '-O3'}[optimization]
            flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', flag,
                     '-I' + str(args.include)]
            if args.asan:
                flags += ['-fsanitize=address,undefined']
        objects = []
        for source in (generated, main):
            obj = folder / (source.stem + '-' + optimization + ('.obj' if args.kind == 'msvc' else '.o'))
            command = [args.compiler, *flags, '/c', source, '/Fo:' + str(obj)] if args.kind == 'msvc' else [
                args.compiler, *flags, '-c', source, '-o', obj]
            compiled = run('compile-' + source.stem + '-' + optimization, command, folder)
            assert compiled.returncode == 0, folder
            objects.append(obj)
        command = [args.compiler, *flags, *objects, *args.library]
        command += ['/Fe:' + str(executable)] if args.kind == 'msvc' else ['-pthread', '-o', executable]
        if args.kind == 'msvc' and args.asan:
            command += ['/link', '/INCREMENTAL:NO']
        linked = run('link-' + optimization, command, folder)
        assert linked.returncode == 0, folder
        ran = run('native-' + optimization, [executable], folder)
        assert (ran.returncode, ran.stdout, ran.stderr) == (0, expected, b''), folder

    hello = args.source_root / 'demos/01-basics/hello.xr'
    oracle = (args.source_root / 'tests/smoke/expected/hello.txt').read_bytes().replace(b'\r\n', b'\n')
    assert hashlib.sha256(hello.read_bytes().replace(b'\r\n', b'\n')).hexdigest() == '9615546fa72feb2d46cd0d5131a15aee3e1c6407a954697fe999e4956bce44e4'
    assert hashlib.sha256(oracle).hexdigest() == 'dc78af31cd0213663f3af6b48b23a3a9e82ee11d593dcc4b10fac47a2cf0d3f3'
    length = root / 'length-existing.xr'
    length.write_text('fn answer()->i64{const s="a"+"b"\nreturn len(s)}\nprint(answer())\n', encoding='utf-8')
    unicode_length = root / 'length-unicode.xr'
    unicode_length.write_text('fn count(text:string)->i64{return len(text)}\n'
        'fn show(n:i64){const text="é\\0${n}"\nprint(len(text))\nprint(count(text))\nprint(text)}\n'
        'show(7)\nshow(-2)\nprint(len(""))\nprint(len("😀e\\u{301}"))\n', encoding='utf-8')
    long_source = args.source_root / 'tests/diff/cases/limits/string_parts_over_64.xr'
    cases = [('length-existing', length, b'2\n'),
             ('length-over-64', long_source, b'140\n'),
             ('length-spill-branches', args.source_root / 'tests/lir/programs/owned-string-spill-branches.xr', b'140\n0\n'),
             ('length-unicode', unicode_length, b'3\n3\n\xc3\xa9\0' b'7\n4\n4\n\xc3\xa9\0-2\n0\n3\n'),
             ('hello', hello, oracle),
             ('join', args.source_root / 'tests/lir/programs/owned-string-join.xr', b'Xray1\n'),
             ('storage', args.source_root / 'tests/lir/programs/owned-string-storage.xr', b'\xc3\xa9\0\nv12\n\n'),
             ('phi', args.source_root / 'tests/lir/programs/owned-string-phi.xr', b'\xc3\xa9\0' b'7\nright-2\n'),
             ('loop-temporary', args.source_root / 'tests/lir/programs/owned-string-loop-temporary.xr', b'n=0\nn=1\nn=2\n'),
             ('loop-branches', args.source_root / 'tests/lir/programs/owned-string-loop-branches.xr', b'left0\nright1\nright2\n'),
             ('sequential', args.source_root / 'tests/lir/programs/owned-string-sequential.xr', b'L-1\nlow-1\nR3\nhigh3\n')]
    for name, source, expected in cases:
        before = source.read_bytes()
        for mode in ('raw', 'sccp', 'inline', 'sccp-inline-inline-sccp'):
            folder = root / (name + '-' + mode)
            folder.mkdir()
            generated = folder / 'generated.c'
            result = run('rbc', [args.driver, source, mode, generated, folder / 'rbc.json', '0', '2', 'normal'], folder)
            assert (result.returncode, result.stdout, result.stderr) == (0, expected * 2, b''), (name, mode, folder)
            if name in ('length-over-64','length-spill-branches'):
                actual=json.loads((folder/'rbc.json').read_text())
                assert actual['rbc_registers_max']<=255 and actual['rbc_spills']>0
                assert actual['rbc_reference_spills']>0
            text = generated.read_text(encoding='utf-8')
            assert '({' not in text and 'XrtString *' in text and 'xrt_string_' in text
            main = folder / 'main.c'
            main.write_text('#include <stdint.h>\nextern int64_t xray_lir_entry(void);\n'
                            'int main(void){if(xray_lir_entry()!=0)return 1;return xray_lir_entry()!=0;}\n', encoding='ascii')
            for optimization in (('Od', 'O1', 'O2', 'Ox') if mode == 'raw' else ('O2',)):
                compile_run(folder,generated,main,optimization,expected * 2)
            records.append({'case': name, 'mode': mode, 'sourceSHA256': hashlib.sha256(before).hexdigest(),
                            'oracleHex': expected.hex(), 'result': 'PASS'})
            if name.startswith('length-'):
                for order in ('program-first', 'outcome-first'):
                    failure = folder / (order + '.json')
                    sink = folder / (order + '-read-only-output')
                    sink.write_bytes(b'')
                    with sink.open('rb') as stream:
                        result = run('length-output-failure-' + order, [args.driver, source, mode,
                            folder / (order + '.c'), failure, '0', '1', order], folder, stdout=stream)
                    marker = b'HOST_ALIVE\r\n' if os.name == 'nt' else b'HOST_ALIVE\n'
                    assert (result.returncode, result.stdout, result.stderr) == (0, None, marker), folder
                    assert sink.read_bytes() == b''
                    facts = json.loads(failure.read_text())
                    assert facts['status'] == 'XR_RBC_OUTPUT_FAILURE' and facts['host_continued']
                    assert facts['owner_path_matches_original'] and not facts['failure_facts']['broken_pipe']
                    records.append({'case': name, 'mode': mode, 'order': order,
                                    'result': 'OUTPUT_FAILURE_CLEANUP_PASS'})
        assert source.read_bytes() == before
    folder = root / 'owned-result'
    folder.mkdir()
    generated = folder / 'generated.c'
    result = run('source-rbc-result',[args.result_driver,generated],folder)
    assert (result.returncode,result.stdout,result.stderr)==(0,b'',b''),folder
    main = folder / 'main.c'
    main.write_text('#include "xray_native_runtime.h"\n#include <stdlib.h>\n#include <string.h>\n'
        'extern XrtString *xray_lir_entry(void);\n'
        'static void consume(void *context){\n'
        ' XrtString *s=xray_lir_entry(); XrtString *retained=xrt_string_retain(s,0);\n'
        ' size_t length=0;const char *bytes; (void)context;\n'
        ' xrt_string_owned_drop(&s);bytes=xrt_string_bytes(retained,&length);\n'
        ' if(length!=5 || memcmp(bytes,"\\303\\251\\000" "73",5))abort();\n'
        ' xrt_string_drop(&retained);}\n'
        'int main(void){const XrtSourceMap map={NULL,0};\n'
        ' xrt_run_standalone(&map,consume,NULL);xrt_run_standalone(&map,consume,NULL);return 0;}\n',encoding='ascii')
    for optimization in ('Od','O1','O2','Ox'):
        compile_run(folder,generated,main,optimization,b'')
    folder = root / 'constant-branch'
    folder.mkdir()
    result = run('constant-branch-rbc', [args.result_driver, '--constant-branch', folder], folder)
    assert (result.returncode, result.stdout, result.stderr) == (0, b'', b''), folder
    main = folder / 'main.c'
    main.write_text('#include "xray_native_runtime.h"\n#include <stdlib.h>\n#include <string.h>\n'
        'extern XrtString *xray_lir_entry(void);\n'
        'static void consume(void *context){XrtString *s=xray_lir_entry();size_t length=0;\n'
        ' const char *bytes=xrt_string_bytes(s,&length);(void)context;\n'
        ' if(length!=4||memcmp(bytes,"kept",4))abort();xrt_string_owned_drop(&s);}\n'
        'int main(void){const XrtSourceMap map={NULL,0};xrt_run_standalone(&map,consume,NULL);\n'
        ' xrt_run_standalone(&map,consume,NULL);return 0;}\n', encoding='ascii')
    for variant in ('raw', 'sccp'):
        child = folder / variant
        child.mkdir()
        for optimization in ('Od', 'O2'):
            compile_run(child, folder / (variant + '.c'), main, optimization, b'')
    loop = root / 'managed-loop.xr'
    loop.write_text('fn repeat(n:i64)->string{var text="";var i=0;while(i<n){text="v${i}";i=i+1};return text}\nprint(repeat(2))\n', encoding='utf-8')
    generated = root / 'managed-loop.c'
    result = run('managed-loop-refusal', [args.source_driver, loop, generated])
    assert result.returncode == 1 and not result.stdout and not generated.exists()
    assert result.stderr.splitlines() == [b'lowering: loop-carried managed String transfer is not supported by LIR lowering yet']
    aliases = [
        ('loop-alias', 'fn alias(n:i64){const text="seed${n}";var i=0;while(i<n){print(text);i=i+1}}\nalias(2)\n',
         b'String alias across blocks without owned parameters'),
        ('loop-parameter', 'fn show(text:string){var i=0;while(i<2){print(text);i=i+1}}\nshow("kept")\n',
         b'String parameter across a loop')]
    for name, text, reason in aliases:
        source = root / (name + '.xr')
        source.write_text(text, encoding='utf-8')
        generated = root / (name + '.c')
        result = run(name + '-refusal', [args.source_driver, source, generated])
        assert result.returncode == 1 and not result.stdout and not generated.exists()
        assert reason in result.stderr, (name, result.stderr)
    folder = root / 'literal-only-loop'
    folder.mkdir()
    source = folder / 'literal-only-loop.xr'
    source.write_text('fn count(n:i64){var i=0;while(i<n){print("tick");i=i+1}}\ncount(2)\n', encoding='utf-8')
    generated = folder / 'generated.c'
    result = run('literal-loop-rbc', [args.driver, source, 'raw', generated, folder / 'rbc.json', 0, 1, 'normal'], folder)
    assert (result.returncode, result.stdout, result.stderr) == (0, b'tick\ntick\n', b''), folder
    main = folder / 'main.c'
    main.write_text('#include <stdint.h>\nextern int64_t xray_lir_entry(void);\nint main(void){return xray_lir_entry()!=0;}\n', encoding='ascii')
    compile_run(folder, generated, main, 'O2', b'tick\ntick\n')
    source = root / 'unpublished-global-reader.xr'
    source.write_text('var n=1\nfn read()->i64{return n}\nvar observed=read()\nprint(observed)\n', encoding='ascii')
    generated = root / 'unpublished.c'
    result = run('unpublished-global-reader', [args.source_driver, source, generated])
    assert result.returncode == 1 and not result.stdout and not generated.exists()
    assert result.stderr.splitlines() == [b'lowering: module initialization call before complete publication is not supported by LIR lowering yet']
    (root / 'result.json').write_text(json.dumps({'state': 'PASS', 'cases': records,
        'libraries': [{'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in args.library]}, indent=2) + '\n')
    print('PASS: owned String complete Source/RBC/C/native', root)


if __name__ == '__main__':
    main()
