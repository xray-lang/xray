"""Run complete signed64/binary64 Source programs and cold public bit witnesses."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'lir'))
from test_lir_carriers_native import _f64_profile, _f64_reject

MODES = ('raw', 'sccp', 'inline', 'sccp-inline-inline-sccp')
PANIC = b'[Uncaught Panic] E0422: numeric conversion is out of range\n'
HOST_ALIVE = b'HOST_ALIVE\r\n' if os.name == 'nt' else b'HOST_ALIVE\n'


def vectors(source):
    literal = (source/'tests/unit/xir/xir_float_vectors.def').read_text()
    bit = r'UINT64_C\(0x([0-9a-f]{16})\)'
    integers = re.findall(r'^XIR_I2F\(64, true, 64, '+bit+', '+bit+r'\)', literal, re.M)
    fractions = re.findall(r'^XIR_F2I\(64, 64, true, '+bit+
        r', XR_XIR_NUMERIC_(OK|RANGE), '+bit+r'\)', literal, re.M)
    bounds = re.findall(r'^XR_F64_I64_BOUNDARY\(([0-9a-f]{16}),([0-9a-f]{16}),(true|false)\)',
        Path(__file__).with_name('f64_i64_boundaries.def').read_text(), re.M)
    assert len(integers) == 49 and len(fractions) == 11 and len(bounds) == 26
    fractions = [(a, b, status == 'RANGE') for a, status, b in fractions]
    fractions += [(a, b, status == 'true') for a, b, status in bounds]
    assert len(fractions)==37 and sum(row[2] for row in fractions)==16
    assert sum(not row[2] for row in fractions)==21
    text = 'static const struct {uint64_t input,expected;} integer_rows[]={\n'
    text += '\n'.join('{UINT64_C(0x'+a+'),UINT64_C(0x'+b+')},' for a, b in integers)+'\n};\n'
    text += 'static const struct {uint64_t input,expected;int range;} fraction_rows[]={\n'
    text += '\n'.join('{UINT64_C(0x'+a+'),UINT64_C(0x'+b+'),'+str(int(r))+'},'
        for a, b, r in fractions)+'\n};\n'
    return text, [i for i, row in enumerate(fractions) if row[2]]


def adapter(cpp=False, generated=False):
    text = '#include "xray_native_runtime.h"\n#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\n#include <fenv.h>\n'
    if cpp:
        text += '#include <type_traits>\n#define ABI(f,t) static_assert(std::is_same<decltype(&f),t>::value,"conversion ABI")\n'
    else:
        text += '#define ABI(f,t) _Static_assert(_Generic(&f,t:1,default:0),"conversion ABI")\n'
    text += 'typedef uint64_t (*IntegerBits)(int64_t);\ntypedef int64_t (*FractionBits)(uint64_t,uint32_t);\n'
    text += 'ABI(xrt_i64_to_f64,IntegerBits);ABI(xrt_f64_to_i64,FractionBits);\n'
    if generated:
        text += ('typedef double (*IntegerSource)(int64_t);\ntypedef int64_t (*FractionSource)(double);\n'
            'extern IntegerSource integer_get(void);\nextern FractionSource fraction_get(void);\n'
            'extern const XrtSourceMap *fraction_map(void);\n')
    text += '#include "vectors.h"\n'
    text += r'''
#if defined(_M_X64) || defined(__x86_64__)
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
static int64_t signed_bits(uint64_t value){
 return value<=INT64_MAX?(int64_t)value:-1-(int64_t)(UINT64_MAX-value);
}
static int environment_matches(int round,int flags,unsigned control){
 if(fegetround()!=round || fetestexcept(FE_ALL_EXCEPT)!=flags)return 0;
#if CONTROLS == 4u
 if(_mm_getcsr()!=control)return 0;
#else
 (void)control;
#endif
 return 1;
}
static uint64_t to_float(int64_t input){
'''
    text += (' double result=integer_get()(input);uint64_t bits;memcpy(&bits,&result,8);return bits;\n}\n' if generated else
             ' return xrt_i64_to_f64(input);\n}\n')
    text += 'static int64_t to_integer(uint64_t input){\n'
    text += (' double value;memcpy(&value,&input,8);return fraction_get()(value);\n}\n' if generated else
             ' return xrt_f64_to_i64(input,1);\n}\n')
    text += r'''
static void on_exit(void){fputs("EXIT_CALLBACK",stderr);}
static void failed(void *data){
 uint64_t *bits=(uint64_t *)data;xrt_print_string("before",6,0);
 (void)to_integer(*bits);fputs("AFTER_CONVERSION",stderr);
}
int main(int argc,char **argv){
#ifdef CONVERSION_RANGE_INDEX
 (void)argc;(void)argv;unsigned long index=CONVERSION_RANGE_INDEX;
#else
 if(argc==2){char *end=NULL;unsigned long index=strtoul(argv[1],&end,10);
  if(end==argv[1] || *end)return 1;
#endif
'''
    text += '  if(index>=37 || !fraction_rows[index].range || atexit(on_exit))return 2;\n'
    if generated:
        text += '  const XrtSourceMap *map=fraction_map();\n'
    else:
        text += '  static const XrtSourceLocation location={"convert.xr",10,7,9,7,19};\n  static const XrtSourceMap descriptor={&location,1};const XrtSourceMap *map=&descriptor;\n'
    text += r'''
  uint64_t bits=fraction_rows[index].input;xrt_run_standalone(map,failed,&bits);return 3;
#ifndef CONVERSION_RANGE_INDEX
 }
 if(argc!=1)return 4;
#endif
 const int rounds[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
 fenv_t original;if(fegetenv(&original))return 5;
#if CONTROLS == 4u
 unsigned original_control=_mm_getcsr();
#endif
 for(unsigned r=0;r<4;++r)for(unsigned c=0;c<CONTROLS;++c){
  if(fesetround(rounds[r]) || feclearexcept(FE_ALL_EXCEPT) || feraiseexcept(FE_DIVBYZERO|FE_INEXACT))return 6;
  unsigned control=0;
#if CONTROLS == 4u
  control=(_mm_getcsr()&~0x8040u)|((c&1u)?0x8000u:0)|((c&2u)?0x40u:0);_mm_setcsr(control);
#endif
  int flags=fetestexcept(FE_ALL_EXCEPT);
  for(unsigned i=0;i<49;++i){
   if(to_float(signed_bits(integer_rows[i].input))!=integer_rows[i].expected ||
      !environment_matches(rounds[r],flags,control))return 7;
  }
  for(unsigned i=0;i<37;++i)if(!fraction_rows[i].range){
   if((uint64_t)to_integer(fraction_rows[i].input)!=fraction_rows[i].expected ||
      !environment_matches(rounds[r],flags,control))return 8;
  }
 }
 if(fesetenv(&original))return 9;
#if CONTROLS == 4u
 _mm_setcsr(original_control);
#endif
 return 0;
}
'''
    return text


def main():
    parser = argparse.ArgumentParser()
    for name in ('driver', 'source-driver', 'bit-driver', 'compiler', 'cxx-compiler', 'source-root', 'include', 'work'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--library', type=Path, nargs='+', required=True)
    parser.add_argument('--kind', choices=('msvc', 'gnu'), required=True)
    parser.add_argument('--msvc-runtime', default='MultiThreaded')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args();args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='conversion-', dir=args.work)).resolve()
    records, products, inputs = [], [], {}

    def run(name, command, exit=0, stdout=b'', stderr=b''):
        assert not any(row['name'] == name for row in records), name
        command = list(map(str, command))
        result = subprocess.run(command, cwd=work, capture_output=True, timeout=120)
        (work/(name+'.stdout')).write_bytes(result.stdout);(work/(name+'.stderr')).write_bytes(result.stderr)
        records.append(dict(name=name, command=command, exit=result.returncode,
            stdoutHex=result.stdout.hex(), stderrHex=result.stderr.hex()))
        (work/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
        assert result.returncode == exit and (stdout is None or result.stdout == stdout) and (
            stderr is None or result.stderr == stderr), (name, work)
        return result

    def flags(cpp=False):
        if args.kind == 'msvc':
            crt = {'MultiThreaded':'/MT','MultiThreadedDebug':'/MTd',
                   'MultiThreadedDLL':'/MD','MultiThreadedDebugDLL':'/MDd'}[args.msvc_runtime]
            values = ['/nologo','/std:c++17' if cpp else '/std:c11','/W4','/WX','/utf-8',crt,'/O2']
            values += ['/clang:-fno-lto'] if 'clang' in args.compiler.name.lower() else ['/GL-']
            if cpp:values += ['/EHsc']
            if args.asan:values += ['/fsanitize=address','/clang:-fsanitize=undefined','/Zi']
            return values
        return ['-std=c++17' if cpp else '-std=c11','-Wall','-Wextra','-Werror','-pedantic','-O2','-fno-lto',
                *(['-fsanitize=address,undefined','-g'] if args.asan else [])]

    def compile_command(source, obj, cpp=False):
        compiler = args.cxx_compiler if cpp else args.compiler
        options = flags(cpp)+[('/I' if args.kind == 'msvc' else '-I')+str(args.include)]
        return [compiler,*options,'/c',source,'/Fo:'+str(obj)] if args.kind == 'msvc' else [compiler,*options,'-c',source,'-o',obj]

    def native(name, sources, cpp=False):
        objects = []
        for i, source in enumerate(sources):
            obj = work/(name+str(i)+('.obj' if args.kind == 'msvc' else '.o'))
            run('compile-'+name+str(i), compile_command(source,obj,cpp), stdout=None);objects.append(obj)
        executable = work/(name+('.exe' if args.kind == 'msvc' else ''))
        options = [flag for flag in flags(cpp) if flag != '/EHsc']
        compiler = args.cxx_compiler if cpp else args.compiler
        command = [compiler,*options,*objects,*args.library]
        command += ['/Fe:'+str(executable),'/link','/INCREMENTAL:NO'] if args.kind == 'msvc' else ['-lm','-pthread','-o',executable]
        run('link-'+name,command,stdout=None)
        products.append(dict(name=name,files=[dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
            for p in [*sources,*objects,executable]]))
        return executable

    profile = _f64_profile(args.compiler,args.kind,flags(),work)
    if profile['supported']:
        generated=work/'pure-convert.c';run('pure-generate',[args.bit_driver,'--pure-c',generated])
        text=generated.read_text()
        assert '#include "xray_native_runtime.h"' in text and 'xrt_i64_to_f64(' in text
        assert 'xrt_run_standalone(' not in text and 'xrl_invoke(' not in text
        pure_main=work/'pure-main.c'
        pure_main.write_text('#include <stdint.h>\n#include <string.h>\n#include <fenv.h>\n'
            'extern double xray_lir_entry(void);\nint main(void){\n'
            ' if(fesetround(FE_DOWNWARD)||feclearexcept(FE_ALL_EXCEPT)||feraiseexcept(FE_INEXACT))return 1;\n'
            ' int flags=fetestexcept(FE_ALL_EXCEPT);double value=xray_lir_entry();uint64_t bits;memcpy(&bits,&value,8);\n'
            ' return bits!=UINT64_C(0x43e0000000000000)||fegetround()!=FE_DOWNWARD||fetestexcept(FE_ALL_EXCEPT)!=flags;}\n',encoding='ascii')
        executable=native('pure-unbound',[generated,pure_main]);run('pure-native',[executable])
    programs = args.source_root/'tests/lir/programs'
    expected = {'i64-to-f64':b'42.0\n-42.0\n1\n1\n','f64-to-i64':b'-42\n42\n0\n-9223372036854775808\n',
        'f64-i64-roundtrip':b'42.0\n-42\n-123\n0\n','f64-i64-side-effect':b'9\n-42\n6\n',
        'f64-i64-spill':b'36855\n'}
    main_c = work/'main.c'
    main_c.write_text('#include <stdint.h>\n#include <stdlib.h>\n#include <stdio.h>\n'
        'extern int64_t xray_lir_entry(void);\nstatic void end(void){fputs("EXIT_CALLBACK",stderr);}\n'
        'int main(void){if(atexit(end))return 2;if(xray_lir_entry()!=0)return 1;return xray_lir_entry()!=0;}\n')
    rows, ranges = vectors(args.source_root);(work/'vectors.h').write_text(rows, encoding='ascii')
    result = dict(state='RUNNING',profile=profile,integerRows=49,originalFractionRows=11,boundaryRows=26)
    for name, oracle in expected.items():
        source = programs/(name+'.xr');inputs[str(source)] = source.read_bytes()
        for mode in MODES:
            label=name+'-'+mode;generated=work/(label+'.c');receipt=work/(label+'.json')
            run('source-'+label,[args.driver,source,mode,generated,receipt,'0','2','normal'],stdout=oracle*2)
            facts = json.loads(receipt.read_text());assert facts['status']=='XR_RBC_OK'
            if name=='f64-i64-spill':assert facts['rbc_spills']>0 and facts['rbc_registers_max']<=255
            if not profile['supported']:
                obj=work/'rejected.obj'
                result.update(state='UNSUPPORTED_TARGET_REJECTED',nativePayloadVectorsExecuted=0,
                    rejection=_f64_reject(compile_command(generated,obj),work,profile['expectedGuard'],obj))
                (work/'result.json').write_text(json.dumps(result,indent=2)+'\n');return
            executable=native(label,[generated,main_c])
            run('native-'+label,[executable],stdout=oracle*2,stderr=b'EXIT_CALLBACK')
    for name in ('f64-i64-range','f64-i64-unused-range'):
        source=programs/(name+'.xr');inputs[str(source)]=source.read_bytes()
        for mode in MODES:
            for order in ('program-first','outcome-first'):
                label=name+'-'+mode+'-'+order;generated=work/(label+'.c');receipt=work/(label+'.json')
                run('source-'+label,[args.driver,source,mode,generated,receipt,'0','1','arithmetic-'+order],
                    stdout=b'before\n',stderr=HOST_ALIVE)
                facts=json.loads(receipt.read_text());assert facts['status']=='XR_RBC_ARITHMETIC_PANIC'
                assert facts['arithmetic_facts']['code']==422 and facts['owner_path_matches_original']
                # Source assigns this conversion the enclosing return's point.
                column=inputs[str(source)].splitlines()[0].index(b'return')+1
                assert facts['failure_location']==[1,column,1,column]
                assert facts['samples'][0]['bits'] is None
                if order=='program-first':
                    executable=native(label,[generated,main_c])
                    run('native-'+label,[executable],exit=1,stdout=b'before\n',stderr=PANIC)
    source=programs/'f64-same-width-unsupported.xr';inputs[str(source)]=source.read_bytes()
    diagnostic=work/'same-width-diagnostic.c'
    marker=b'C emission: operation has no admitted portable C implementation'
    run('same-width-diagnostic',[args.source_driver,source,diagnostic],exit=1,
        stderr=marker+(b'\r\n' if os.name=='nt' else b'\n'))
    assert not diagnostic.exists()
    for mode in MODES:
        output=work/('same-width-'+mode+'.c');receipt=work/('same-width-'+mode+'.json')
        rejected=run('same-width-'+mode,[args.driver,source,mode,output,receipt,'0','1','normal'],
                     exit=1,stdout=b'',stderr=None)
        assert b'xr_lir_emit_c' in rejected.stderr and not output.exists() and not receipt.exists()
    source=programs/'f64-i64-module-slot-unsupported.xr';inputs[str(source)]=source.read_bytes()
    marker=b'module slot without a direct same-block i64 literal is not supported by LIR lowering yet'
    for mode in MODES:
        output=work/('module-slot-'+mode+'.c');receipt=work/('module-slot-'+mode+'.json')
        rejected=run('module-slot-'+mode,[args.driver,source,mode,output,receipt,'0','1','normal'],
            exit=1,stdout=b'',stderr=None)
        assert b'lower: '+marker in rejected.stderr and not output.exists() and not receipt.exists()
    for cpp in (False,True):
        label='public-cpp' if cpp else 'public-c';path=work/(label+('.cpp' if cpp else '.c'))
        path.write_text(adapter(cpp),encoding='ascii');executable=native(label,[path],cpp)
        run('native-'+label,[executable])
        for index in ranges:run('range-'+label+str(index),[executable,str(index)],exit=1,stdout=b'before\n',stderr=PANIC)
    gateways=[]
    for name,result_type,parameter in (('integer','double','int64_t'),('fraction','int64_t','double')):
        generated=work/(('i64-to-f64' if name=='integer' else 'f64-to-i64')+'-raw.c')
        functions=re.findall(r'static '+result_type+r' (xrl_f\d+)\('+parameter+r' p0\) \{',generated.read_text())
        assert len(functions)==1,functions
        path=work/(name+'-gateway.c')
        path.write_text('#define xray_lir_entry '+name+'_entry\n#define xrt_standalone_source_map '+name+'_source_map\n'
            '#include "'+generated.name+'"\ntypedef '+result_type+' (*Callable)('+parameter+');\n'
            '_Static_assert(_Generic(&'+functions[0]+',Callable:1,default:0),"Source conversion ABI");\n'
            'Callable '+name+'_get(void){return &'+functions[0]+';}\n'+
            ('const XrtSourceMap *fraction_map(void){return &fraction_source_map;}\n' if name=='fraction' else ''),encoding='ascii')
        gateways.append(path)
    path=work/'source-adapter.c';path.write_text(adapter(generated=True),encoding='ascii')
    executable=native('source-callees',[*gateways,path]);run('native-source-callees',[executable])
    for index in ranges:run('range-source-callees'+str(index),[executable,str(index)],exit=1,stdout=b'before\n',stderr=PANIC)
    assert all(Path(p).read_bytes()==data for p,data in inputs.items())
    result.update(state='PASS',sourcePrograms=len(inputs),modes=4,publicClients=2,publicRangeVectors=len(ranges),
        products=products,sourceSHA256={p:hashlib.sha256(data).hexdigest() for p,data in inputs.items()},
        boundary='Build closure only; official installed/moved component qualification is separate.')
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(work)


if __name__ == '__main__':main()
