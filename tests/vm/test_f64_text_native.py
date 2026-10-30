"""Consume complete Source F64 text in generated C and public String clients."""
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


def vectors(source):
    path = source / 'tests/unit/xir/xir_float_format_vectors.def'
    data = path.read_bytes()
    assert hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest() == '9840d237849bd7232bdba37beeec7b850c1637fcf2b37573f6109a5b0c32f20e'
    rows = re.findall(r'^XIR_FLOAT_FORMAT\(64, UINT64_C\(0x([0-9a-f]{16})\), "([^"]+)"\)',
                      data.decode('ascii'), re.M)
    assert len(rows) == 290
    return '\n'.join('{UINT64_C(0x' + bits + '),' + json.dumps(text) + '},' for bits, text in rows)


def adapter(public=False, cpp=False):
    text = '#include "xray_native_runtime.h"\n#include <string.h>\n#include <stdlib.h>\n#include <fenv.h>\n'
    if public:
        text += ('#include <type_traits>\nstatic_assert(std::is_same<decltype(&xrt_string_f64),'
                 'XrtString *(*)(uint64_t,uint32_t)>::value,"public text ABI");\n' if cpp else
                 '_Static_assert(_Generic(&xrt_string_f64,XrtString *(*)(uint64_t,uint32_t):1,default:0),"public text ABI");\n')
    else:
        text += ('typedef XrtString *(*Text)(double);\nextern Text f64_text_get(void);\n'
                 'extern const XrtSourceMap *f64_text_source_map(void);\n')
    text += r'''
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
typedef struct Witness { uint64_t bits; const char *text; } Witness;
static const Witness rows[]={
#include "vectors.h"
};
static int environment_matches(int round,int flags,unsigned control){
 if(fegetround()!=round || fetestexcept(FE_ALL_EXCEPT)!=flags)return 0;
#if CONTROL_CASES == 4u
 if(_mm_getcsr()!=control)return 0;
#else
 (void)control;
#endif
 return 1;
}
static void equal_text(const XrtString *value,const char *expected){
 size_t length=0;const char *bytes=xrt_string_bytes(value,&length);
'''
    text += (' if(length!=strlen(expected) || memcmp(bytes,expected,length))abort();\n}\n' if public else
             ' if(length!=strlen(expected)+2 || memcmp(bytes,"v=",2) || memcmp(bytes+2,expected,length-2))abort();\n}\n')
    if public:
        text += ('static void invoke(void *data){const Witness *row=(const Witness *)data;\n'
                 ' XrtString *value=xrt_string_f64(row->bits,0);equal_text(value,row->text);\n'
                 ' xrt_string_drop(&value);if(value)abort();}\n')
    else:
        text += ('static void invoke(void *data){const Witness *row=(const Witness *)data;\n'
                 ' double input;memcpy(&input,&row->bits,sizeof(input));\n'
                 ' XrtString *value=f64_text_get()(input);equal_text(value,row->text);\n'
                 ' xrt_string_drop(&value);if(value)abort();}\n')
    text += r'''
int main(void){
 const int rounds[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
 fenv_t original;if(fegetenv(&original))return 1;
#if CONTROL_CASES == 4u
 unsigned original_control=_mm_getcsr();
#endif
 for(unsigned round=0;round<4;++round)for(unsigned mode=0;mode<CONTROL_CASES;++mode){
  if(fesetround(rounds[round]) || feclearexcept(FE_ALL_EXCEPT) || feraiseexcept(FE_DIVBYZERO|FE_INEXACT))return 2;
  unsigned control=0;
#if CONTROL_CASES == 4u
  control=(_mm_getcsr()&~0x8040u) | ((mode&1)?0x8000u:0) | ((mode&2)?0x40u:0);
  _mm_setcsr(control);
#endif
  int flags=fetestexcept(FE_ALL_EXCEPT);
  for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);++i)for(unsigned run=0;run<2;++run){
'''
    if public:
        text += ('   static const XrtSourceMap locations={NULL,0};Witness row=rows[i];\n'
                 '   xrt_run_standalone(&locations,invoke,&row);\n')
    else:
        text += ('   Witness row=rows[i];\n'
                 '   xrt_run_standalone(f64_text_source_map(),invoke,&row);\n')
    text += r'''
   if(!environment_matches(rounds[round],flags,control))return 5;
  }
 }
 if(fesetenv(&original))return 6;
#if CONTROL_CASES == 4u
 _mm_setcsr(original_control);
#endif
 return 0;
}
'''
    return text


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'pipeline-driver', 'compiler', 'cxx-compiler', 'source-root', 'include', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='text-', dir=args.work)).resolve()
    records = []

    def run(name, command, folder=work):
        result = subprocess.run(list(map(str, command)), cwd=folder, capture_output=True, timeout=60)
        (folder / (name + '.stdout')).write_bytes(result.stdout)
        (folder / (name + '.stderr')).write_bytes(result.stderr)
        records.append(dict(name=name, command=list(map(str, command)), cwd=str(folder), exit=result.returncode))
        (work / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
        assert result.returncode == 0 and not result.stderr, (name, work)
        return result.stdout

    observation = json.loads(run('source', [args.driver, '--f64-text-c', work]))
    assert all(observation[key] == value for key, value in
               dict(sourceBytes=138, oracleBytes=24, independentVectors=290, rbcEntries=580).items())
    assert observation['callee'] > 0
    source = args.source_root / 'tests/lir/programs/f64-text.xr'
    before = source.read_bytes()
    assert hashlib.sha256(before.replace(b'\r\n', b'\n')).hexdigest() == '89807d8c49e885d01b35402c03697496dafef7d1f5ef41ba86f4dac8d486e43f'
    expected = b'v=3.5\n-0.0\ninf -inf nan\n'
    (work / 'vectors.h').write_text(vectors(args.source_root), encoding='ascii')
    (work / 'gateway.c').write_text('#include "whole.c"\n'
        'typedef XrtString *(*Text)(double);\n'
        f'_Static_assert(_Generic(&xrl_f{observation["callee"]},Text:1,default:0),"Source text ABI");\n'
        f'Text f64_text_get(void){{return &xrl_f{observation["callee"]};}}\n'
        'const XrtSourceMap *f64_text_source_map(void){return &xrt_standalone_source_map;}\n', encoding='ascii')
    (work / 'adapter.c').write_text(adapter(), encoding='ascii')

    def options(cpp=False):
        if args.kind == 'msvc':
            crt = {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                   'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'}[args.msvc_runtime]
            flags = ['/nologo', '/std:c++17' if cpp else '/std:c11', '/W4', '/WX', '/utf-8', crt, '/O2',
                     '/I' + str(args.include), '/I' + str(work)]
            flags += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
            if cpp: flags += ['/EHsc']
            if args.asan: flags += ['/fsanitize=address', '/clang:-fsanitize=undefined', '/Zi']
            return flags
        return ['-std=c++17' if cpp else '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2', '-fno-lto',
                '-I' + str(args.include), '-I' + str(work), *(['-fsanitize=address,undefined', '-g'] if args.asan else [])]

    def compile_command(compiler, flags, path, obj):
        return [compiler, *flags, '/c', path, '/Fo:' + str(obj)] if args.kind == 'msvc' else [compiler, *flags, '-c', path, '-o', obj]

    flags = options()
    profile = _f64_profile(args.compiler, args.kind, flags, work)
    result = dict(observation=observation, profile=profile, sourceSHA256=hashlib.sha256(before).hexdigest(),
                  oracleHex=expected.hex(), sourceRbcEntries=580)
    if not profile['supported']:
        obj = work / 'rejected.obj'
        result.update(state='UNSUPPORTED_TARGET_REJECTED', nativePayloadVectorsExecuted=0,
            rejection=_f64_reject(compile_command(args.compiler, flags, work / 'gateway.c', obj),
                                  work, profile['expectedGuard'], obj))
        (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        return

    def native(name, sources, cpp=False, expected_stdout=b'', private=False):
        compiler = args.cxx_compiler if cpp else args.compiler
        flags = options(cpp)
        if private:
            flags += [('/I' if args.kind == 'msvc' else '-I') + str(args.source_root / 'src')]
            if args.kind == 'msvc':
                flags += ['/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS']
        objects = []
        for i, path in enumerate(sources):
            obj = work / (name + str(i) + ('.obj' if args.kind == 'msvc' else '.o'))
            run('compile-' + name + str(i), compile_command(compiler, flags, path, obj))
            objects.append(obj)
        executable = work / (name + ('.exe' if args.kind == 'msvc' else ''))
        link_flags = [flag for flag in flags if flag != '/EHsc']
        command = [compiler, *link_flags, *objects, *args.library]
        command += ['/Fe:' + str(executable), '/link', '/INCREMENTAL:NO'] if args.kind == 'msvc' else ['-lm', '-pthread', '-o', executable]
        run('link-' + name, command)
        assert run('run-' + name, [executable]) == expected_stdout

    def normal_frame_observer(generated, name):
        observer = work / ('normal-frame-' + name + '.c')
        observer.write_text('#include "runtime/native/xnative_execution.h"\n#include <stdlib.h>\n'
            'static XrtString *watched;static unsigned created,dropped;\n'
            'static XrtString *observed_f64(uint64_t bits,uint32_t source){\n'
            ' XrtString *value=xrt_string_f64(bits,source);\n'
            ' if(bits==UINT64_C(0x8000000000000000)){if(watched)abort();watched=value;++created;}\n'
            ' return value;}\n'
            'static void observed_drop(XrtString **value){\n'
            ' if(*value && *value==watched){watched=NULL;++dropped;}xrt_string_drop(value);}\n'
            '#define xrt_string_f64(bits,source) observed_f64((bits),(source))\n'
            '#define xrt_string_drop(value) observed_drop(value)\n'
            f'#include "{generated.name}"\n'
            '#undef xrt_string_f64\n#undef xrt_string_drop\n'
            'typedef struct Observation{XrtStringRoot strings;unsigned visits;} Observation;\n'
            'static void observe(void *data){Observation *o=data;int64_t result=99;xrl_invoke(&result);\n'
            ' if(result || watched || o->strings.owners || created!=o->visits+1 || dropped!=created)abort();\n'
            ' ++o->visits;}\n'
            'int main(void){Observation o={0};XrtLocationOwner *locations=NULL;XrtExecutionFailure failure={0};\n'
            ' if(xrt_locations_create(xrt_standalone_source_map.locations,xrt_standalone_source_map.count,&locations)!=XRT_LOCATION_OK)return 1;\n'
            ' for(unsigned run=0;run<2;++run){\n'
            '  if(xrt_run_embedded_owned(locations,&failure,observe,&o,&o.strings)!=XRT_EXECUTION_COMPLETED)return 2;\n'
            '  if(o.visits!=run+1 || o.strings.owners || o.strings.domain)return 3;\n'
            ' }\n xrt_locations_release(locations);return 0;}\n', encoding='ascii')
        native('normal-frame-' + name, [observer], expected_stdout=expected * 2, private=True)

    native('local', [work / 'gateway.c', work / 'adapter.c'])
    for cpp in (False, True):
        path = work / ('public.cpp' if cpp else 'public.c')
        path.write_text(adapter(True, cpp), encoding='ascii')
        native('public-cpp17' if cpp else 'public-c11', [path], cpp)
    main_c = work / 'main.c'
    main_c.write_text('#include <stdint.h>\nextern int64_t xray_lir_entry(void);\n'
                      'int main(void){if(xray_lir_entry()!=0)return 1;return xray_lir_entry()!=0;}\n', encoding='ascii')
    for mode in ('raw', 'sccp', 'inline', 'sccp-inline-inline-sccp'):
        output = work / (mode + '.c')
        assert run('source-' + mode, [args.pipeline_driver, source, mode, output,
            work / (mode + '.json'), '0', '2', 'normal']) == expected * 2
        native(mode, [output, main_c], expected_stdout=expected * 2)
        normal_frame_observer(output, mode)
    spill_source = args.source_root / 'tests/lir/programs/f64-text-spill.xr'
    spill_before = spill_source.read_bytes()
    spill_modes = []
    for mode in ('raw', 'sccp', 'inline', 'sccp-inline-inline-sccp'):
        name = 'spill-' + mode
        output = work / (name + '.c')
        receipt = work / (name + '.json')
        assert run('source-' + name, [args.pipeline_driver, spill_source, mode, output,
            receipt, '0', '2', 'normal']) == b'1080\n1080\n'
        facts = json.loads(receipt.read_text())
        assert facts['rbc_registers_max'] <= 255 and facts['rbc_reference_spills'] > 0 and facts['rbc_spills'] > 0
        native(name, [output, main_c], expected_stdout=b'1080\n1080\n')
        spill_modes.append(dict(mode=mode, registers=facts['rbc_registers_max'],
                               spills=facts['rbc_spills'], referenceSpills=facts['rbc_reference_spills']))
    assert spill_source.read_bytes() == spill_before
    fast = flags + (['/fp:fast'] if args.kind == 'msvc' else ['-ffast-math'])
    rejected = work / 'fast.obj'
    result['fastRejection'] = _f64_reject(compile_command(args.compiler, fast, work / 'gateway.c', rejected),
        work, 'F64_TRANSPORT_REQUIRES_NONFAST_FLOATING_POINT_MODE', rejected)
    assert source.read_bytes() == before
    result.update(state='PASS', independentVectors=290, roundingModes=4, controlCases=4,
                  entriesPerImage=2, fullSourceModes=4, publicClients=2, installedClients=0,
                  normalFrameModes=4, directPrintExplicitDropPerEntry=1, preDrainOwnerEmpty=True,
                  typedSpillModes=spill_modes, typedSpillSourceSHA256=hashlib.sha256(spill_before).hexdigest(),
                  boundary='Public clients use this build closure; official installed/moved component is separate.')
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(work)


if __name__ == '__main__':
    main()
