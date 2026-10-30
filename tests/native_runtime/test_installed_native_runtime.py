"""Install and move the native component before compiling complete cold clients."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'c-driver', 'pipeline-driver', 'result-driver', 'work'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--eligible', choices=('0', '1'), required=True)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='installed-', dir=args.work)).resolve()
    commands, cases, clients = [], {}, []

    def run(label, command, expected=0, folder=work):
        assert not any(row['name'] == label for row in commands), ('duplicate log label', label)
        result = subprocess.run(list(map(str, command)), cwd=folder, capture_output=True, timeout=180)
        (work/(label + '.stdout')).write_bytes(result.stdout)
        (work/(label + '.stderr')).write_bytes(result.stderr)
        commands.append(dict(name=label, command=list(map(str, command)), cwd=str(folder), exit=result.returncode,
                             stdoutSHA256=hashlib.sha256(result.stdout).hexdigest(), stderrSHA256=hashlib.sha256(result.stderr).hexdigest()))
        (work/'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if expected == 'failure':
            assert result.returncode != 0, (label, work)
        else:
            assert result.returncode == expected, (label, work)
        return result

    prefix = work/'prefix'
    if args.eligible == '0':
        rejected = run('install-unsupported', ['cmake','--install',args.build,'--component','XrayNativeRuntime','--prefix',prefix], 'failure')
        assert b'XrayNativeRuntime requires qualified' in rejected.stdout + rejected.stderr
        rejected = run('build-unsupported', ['cmake','--build',args.build,'--target','native-runtime-bundle','-j2'], 'failure')
        assert b'XrayNativeRuntime requires qualified' in rejected.stdout + rejected.stderr
        assert not list(prefix.rglob('XrayNativeRuntimeConfig.cmake'))
        (work/'summary.json').write_text(json.dumps(dict(state='UNSUPPORTED_TUPLE_REJECTED',
            installExit=commands[0]['exit'], bundleExit=commands[1]['exit'], positiveClientsExecuted=0), indent=2) + '\n')
        print(work)
        return
    run('build-bundle', ['cmake','--build',args.build,'--target','native-runtime-bundle','-j2'])
    run('install', ['cmake', '--install', args.build, '--component', 'XrayNativeRuntime', '--prefix', prefix])
    assert not (prefix/'share/xray/install/payload-manifest.json').exists()
    moved = work/'moved-prefix'
    prefix.rename(moved)
    runtime = moved/'lib/xray/native-runtime/abi1/x86_64-windows-msvc/msvc-release-mt-san0'
    manifest = json.loads((runtime/'native_runtime_manifest.json').read_text())
    assert manifest['sourceDirty'] is False and manifest['sanitizers'] == 0
    project = work/'clients';project.mkdir()
    shutil.copyfile(Path(__file__).with_name('installed')/'CMakeLists.txt', project/'CMakeLists.txt')

    def add(name, sources, stdout=b'', exit=0, stderr=b''):
        clients.append(name)
        cases[name] = dict(sources=[str(p) for p in sources], stdoutHex=stdout.hex(), stderrHex=stderr.hex(), exit=exit)

    source_inputs = {}

    def source_input(relative, length, digest):
        source = args.source/relative
        raw = source.read_bytes()
        # Verify fixed fixture content across Git checkout EOL settings. The
        # production drivers below still receive this untouched original path.
        canonical = raw.replace(b'\r\n', b'\n')
        assert len(canonical) == length and hashlib.sha256(canonical).hexdigest() == digest
        source_inputs[relative] = dict(path=str(source), rawBytes=len(raw),
            rawSHA256=hashlib.sha256(raw).hexdigest(), CRLF=raw.count(b'\r\n'),
            bareLF=raw.count(b'\n')-raw.count(b'\r\n'), canonicalLFBytes=length,
            canonicalLFSHA256=digest)
        return source

    expected = args.source/'tests/diff/cases/semantics/output/print_terminator_exact.xr.expected'
    oracle = expected.read_bytes().replace(b'\r\n', b'\n')
    assert len(oracle) == 31 and hashlib.sha256(oracle).hexdigest() == 'cc9ebe4b7bd9fdc01c27cc79ff6c43c1e90e78115cad7795157f67b316d11c21'
    literal = source_input('tests/diff/cases/semantics/output/print_terminator_exact.xr', 497,
        '32f13c28e415c770c81bd0eafbfb7d4008b8bdf192f7cb6cade5c01f2845a6a2')
    for optimized in (False, True):
        name = 'literal_opt' if optimized else 'literal'
        folder = project/name;folder.mkdir()
        run(name + '-generate', [args.c_driver, literal, folder/'generated.c', *(['--optimize'] if optimized else [])])
        (folder/'main.c').write_text('#include <stdint.h>\nextern int64_t xray_lir_entry(void);\n'
            'int main(void){if(xray_lir_entry()!=0)return 1;return xray_lir_entry()!=0;}\n', encoding='ascii')
        add(name, [folder/'generated.c', folder/'main.c'], oracle*2)
    sources = [('long_string', 'tests/diff/cases/limits/string_parts_over_64.xr', 876,
                '0a6919aec0be3b82c556aa7237e2d99b2e272b4792333a78e8395139d23c8f34', b'140\n'),
               ('spill_branches', 'tests/lir/programs/owned-string-spill-branches.xr', 959,
                'ee150edc9acc304a7f12185f43c41fd326f47f7a18e7d38eceffa0ba9c36e3a9', b'140\n0\n'),
               ('unicode_nul', 'tests/lir/programs/owned-string-storage.xr', 150,
                '5b53b0ed4708c139c981c76e91d60d9326d612d7bb80ca9b61ba2d09d498a88b', b'\xc3\xa9\0\nv12\n\n'),
               ('text_whole', 'tests/lir/programs/f64-text.xr', 138,
                '89807d8c49e885d01b35402c03697496dafef7d1f5ef41ba86f4dac8d486e43f', b'v=3.5\n-0.0\ninf -inf nan\n')]
    for name, relative, length, digest, stdout in sources:
        source = source_input(relative, length, digest)
        for mode in ('raw', 'sccp', 'inline', 'sccp-inline-inline-sccp'):
            client = name + '_' + mode.replace('-', '_')
            folder = project/client;folder.mkdir()
            generated = folder/'generated.c'
            result = run(client + '-generate', [args.pipeline_driver, source, mode, generated, folder/'rbc.json', '0', '2', 'normal'])
            assert result.stdout == stdout*2 and not result.stderr
            if name in ('long_string', 'spill_branches'):
                receipt = json.loads((folder/'rbc.json').read_text())
                assert receipt['rbc_registers_max'] <= 255 and receipt['rbc_spills'] > 0 and receipt['rbc_reference_spills'] > 0
            (folder/'main.c').write_text('#include <stdint.h>\nextern int64_t xray_lir_entry(void);\n'
                'int main(void){if(xray_lir_entry()!=0)return 1;return xray_lir_entry()!=0;}\n', encoding='ascii')
            add(client, [generated, folder/'main.c'], stdout*2)
    folder = project/'owned_result';folder.mkdir()
    run('owned-result-generate', [args.result_driver, folder/'generated.c'])
    (folder/'main.c').write_text('#include "xray_native_runtime.h"\n#include <stdlib.h>\n#include <string.h>\n'
        'extern XrtString *xray_lir_entry(void);\nstatic void consume(void *context){\n'
        ' XrtString *s=xray_lir_entry();XrtString *retained=xrt_string_retain(s,0);\n'
        ' size_t length=0;(void)context;xrt_string_owned_drop(&s);\n'
        ' const char *bytes=xrt_string_bytes(retained,&length);\n'
        ' if(length!=5||memcmp(bytes,"\\303\\251\\000" "73",5))abort();xrt_string_drop(&retained);}\n'
        'int main(void){const XrtSourceMap map={NULL,0};xrt_run_standalone(&map,consume,NULL);\n'
        ' xrt_run_standalone(&map,consume,NULL);return 0;}\n', encoding='ascii')
    add('owned_result', [folder/'generated.c', folder/'main.c'])
    family = project/'family';family.mkdir()
    observed = json.loads(run('family-generate', [args.result_driver, '--f64-family-c', family]).stdout)
    module_path = args.source/'tests/vm/test_f64_family_native.py'
    spec = importlib.util.spec_from_file_location('native_family_witness', module_path)
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    golden = args.source/'tests/unit/xir/xir_float_arithmetic_vectors.def'
    assert hashlib.sha256(golden.read_bytes().replace(b'\r\n', b'\n')).hexdigest() == 'f55ebc6c5529d16793607901910a6707d65b2df8a311b1820a30b218ee52805c'
    rows = re.findall(r'^XIR_FLOAT_ARITHMETIC\((32|64), XR_XIR_FLOAT_(\w+), UINT64_C\(0x([0-9a-f]{16})\), UINT64_C\(0x([0-9a-f]{16})\), UINT64_C\(0x([0-9a-f]{16})\)\)', golden.read_text(), re.M)
    assert len(rows) == 6432
    rows = [row for row in rows if row[0] == '64'];assert len(rows) == 3216
    operators = dict(ADD=0, SUBTRACT=1, MULTIPLY=2, DIVIDE=3)
    arithmetic = '\n'.join('{' + str(operators[op]) + ',' + ','.join('UINT64_C(0x' + value + ')' for value in (a,b,c)) + '},' for _,op,a,b,c in rows)
    values = re.findall(r'XR_F64_VALUE\(([0-9a-f]{16}), (-?\d+)\)', (args.source/'tests/vm/f64_compare_values.def').read_text())
    assert len(values) == 26
    order = '\n'.join('{UINT64_C(0x' + bits + '),' + rank + '},' for bits,rank in values)
    for mode in ('arithmetic', 'relations', 'public'):
        for cpp in (False, True):
            name = 'f64_' + mode + ('_cpp' if cpp else '_c')
            folder = project/name;folder.mkdir()
            (folder/'arithmetic-values.h').write_text(arithmetic, encoding='ascii')
            (folder/'order-values.h').write_text(order, encoding='ascii')
            text = module.adapter(mode, cpp)
            if cpp and mode != 'public':
                text = text.replace('extern Arithmetic arithmetic_get(void);', 'extern "C" Arithmetic arithmetic_get(void);')
                text = text.replace('extern Relations relations_get(void);', 'extern "C" Relations relations_get(void);')
                text = text.replace('extern int64_t root_run(void);', 'extern "C" int64_t root_run(void);')
            client = folder/('adapter.cpp' if cpp else 'adapter.c');client.write_text(text, encoding='ascii')
            objects = [client]
            if mode != 'public':
                shutil.copyfile(family/(mode + '.c'), folder/'generated.c')
                callee = observed['arithmeticCallee' if mode == 'arithmetic' else 'relationsCallee']
                signature = 'double (*Callable)(int64_t,double,double)' if mode == 'arithmetic' else 'int64_t (*Callable)(double,double)'
                (folder/'gateway.c').write_text('#include "generated.c"\ntypedef ' + signature + ';\n'
                    f'_Static_assert(_Generic(&xrl_f{callee},Callable:1,default:0),"Source ABI");\n'
                    f'Callable {mode}_get(void){{return &xrl_f{callee};}}\n'
                    'int64_t root_run(void){return xray_lir_entry();}\n', encoding='ascii')
                objects.append(folder/'gateway.c')
            add(name, objects, b'14\n' if mode == 'relations' else b'')
    text_family=project/'text-family';text_family.mkdir()
    text_observed=json.loads(run('text-family-generate',[args.result_driver,'--f64-text-c',text_family]).stdout)
    assert text_observed['sourceBytes']==138 and text_observed['independentVectors']==290 and text_observed['rbcEntries']==580
    text_path=args.source/'tests/vm/test_f64_text_native.py'
    text_spec=importlib.util.spec_from_file_location('native_text_witness',text_path)
    text_module=importlib.util.module_from_spec(text_spec);text_spec.loader.exec_module(text_module)
    text_vectors=text_module.vectors(args.source)
    for public in (False,True):
        for cpp in (False,True):
            name='f64_text_'+('public' if public else 'source')+('_cpp' if cpp else '_c')
            folder=project/name;folder.mkdir();(folder/'vectors.h').write_text(text_vectors,encoding='ascii')
            text=text_module.adapter(public,cpp)
            if cpp and not public:
                text=text.replace('extern Text f64_text_get(void);','extern "C" Text f64_text_get(void);')
                text=text.replace('extern const XrtSourceMap *f64_text_source_map(void);',
                                  'extern "C" const XrtSourceMap *f64_text_source_map(void);')
            client=folder/('adapter.cpp' if cpp else 'adapter.c');client.write_text(text,encoding='ascii')
            objects=[client]
            if not public:
                shutil.copyfile(text_family/'whole.c',folder/'generated.c')
                callee=text_observed['callee']
                (folder/'gateway.c').write_text('#include "generated.c"\ntypedef XrtString *(*Text)(double);\n'
                    f'_Static_assert(_Generic(&xrl_f{callee},Text:1,default:0),"Source text ABI");\n'
                    f'Text f64_text_get(void){{return &xrl_f{callee};}}\n'
                    'const XrtSourceMap *f64_text_source_map(void){return &xrt_standalone_source_map;}\n',encoding='ascii')
                objects.append(folder/'gateway.c')
            add(name,objects)
    panic = project/'panic.c'
    panic.write_text('#include "xray_native_runtime.h"\n#include <stdlib.h>\n#include <stdio.h>\n'
        'const XrtSourceMap xrt_standalone_source_map={NULL,0};\n'
        'static void callback(void){fputs("EXIT_CALLBACK",stderr);}\n'
        'static void root(void *p){(void)p;XrtString *a=xrt_string_new("\\303\\251\\000",3,0);\n'
        ' XrtString *b=xrt_string_retain(a,0);(void)b;xrt_print_string("header",6,0);\n'
        ' xrt_arithmetic_panic(421,0);}\n'
        'int main(void){if(atexit(callback))return 2;xrt_run_standalone(&xrt_standalone_source_map,root,NULL);\n'
        ' fputs("AFTER_CALL",stderr);return 77;}\n', encoding='ascii')
    add('panic', [panic], b'header\n', 1, b'[Uncaught Panic] E0421: modulo by zero\n')
    for name,case in list(cases.items()):
        if name.startswith('f64_'):
            continue
        original = Path(case['sources'][-1])
        cpp = original.with_suffix('.cpp')
        text = original.read_text(encoding='ascii')
        text = text.replace('extern int64_t xray_lir_entry(void);', 'extern "C" int64_t xray_lir_entry(void);')
        text = text.replace('extern XrtString *xray_lir_entry(void);', 'extern "C" XrtString *xray_lir_entry(void);')
        cpp.write_text(text, encoding='ascii')
        add(name + '_cpp', [*[Path(p) for p in case['sources'][:-1]],cpp],
            bytes.fromhex(case['stdoutHex']), case['exit'], bytes.fromhex(case['stderrHex']))
    (project/'cases.cmake').write_text('set(CLIENTS ' + ' '.join(clients) + ')\n' + ''.join(
        'set(' + name + '_SOURCES ' + ' '.join('"' + path.replace('\\','/') + '"' for path in case['sources']) + ')\n' for name,case in cases.items()), encoding='utf-8')
    external = work/'external-build'

    def configure(label, prefix_path, target=external, extra=()):
        return run(label, ['cmake','-S',project,'-B',target,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
                          '-DCMAKE_C_COMPILER=cl','-DCMAKE_CXX_COMPILER=cl','-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
                          '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_PREFIX_PATH=' + str(prefix_path),*extra])

    def reject_config(label, prefix_path=moved, extra=()):
        result = run(label, ['cmake','-S',project,'-B',work/('r' + str(len(commands))),'-G','Ninja',
            '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=cl','-DCMAKE_CXX_COMPILER=cl',
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded','-DCMAKE_PREFIX_PATH=' + str(prefix_path),*extra], 'failure')
        assert b'XrayNativeRuntime' in result.stderr, label
        return result

    configure('consumer-configure', moved)
    run('consumer-build', ['cmake','--build',external,'-j2'])
    for name, case in cases.items():
        result = run('execute-' + name, [external/(name + '.exe')], case['exit'])
        assert result.stdout.hex() == case['stdoutHex'] and result.stderr.hex() == case['stderrHex'], name
    cold_commands = (external/'compile_commands.json').read_text()
    normalized_commands = cold_commands.replace('\\','/')
    assert (args.source/'include').as_posix() not in normalized_commands
    assert (args.source/'src').as_posix() not in normalized_commands
    producer_inputs = json.loads((args.build/'native-runtime-inputs.json').read_text())
    assert all(row['file'].replace('\\','/') not in normalized_commands for row in producer_inputs['archives'])
    includes = re.findall(r'(?:^|\s)[/-](?:external:)?I(?:"([^"]+)"|(\S+))', cold_commands, re.M)
    assert includes and all(Path(first or second).resolve() == (runtime/'include').resolve() for first,second in includes)
    assert str(moved).replace('\\','/') in cold_commands.replace('\\','/')
    run('legacy-publisher-on-independent-prefix', ['cmake','-DCMAKE_INSTALL_PREFIX=' + str(moved),
        '-DCMAKE_INSTALL_COMPONENT=XrayCore','-P',args.build/'generated/install/GeneratePayloadManifest.cmake'])
    legacy = json.loads((moved/'share/xray/install/payload-manifest.json').read_text())
    assert legacy['files'] == [] and legacy['components'] == []
    negative = []
    package = moved/'lib/cmake/XrayNativeRuntime'
    for index,row in enumerate(manifest['files']):
        path = runtime/row['path'];original = path.read_bytes()
        for change in ('missing', 'tamper'):
            if change == 'missing':path.unlink()
            else:path.write_bytes(original + b'!')
            label = 'member-' + str(index) + '-' + change
            result = run(label, ['cmake','-S',project,'-B',work/('reject-' + label),'-G','Ninja',
                '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=cl','-DCMAKE_CXX_COMPILER=cl',
                '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded','-DCMAKE_PREFIX_PATH=' + str(moved)], 'failure')
            assert b'XrayNativeRuntime member' in result.stderr, label
            path.write_bytes(original)
            negative.append(label)
    manifest_path = runtime/'native_runtime_manifest.json'
    config_path = package/'XrayNativeRuntimeConfig.cmake'
    original_manifest, original_config = manifest_path.read_bytes(), config_path.read_bytes()
    for label in ('manifest-missing', 'manifest-tamper', 'wrong-abi', 'wrong-namespace',
                  'wrong-provider', 'wrong-version', 'wrong-target', 'wrong-crt', 'wrong-profile',
                  'wrong-sanitizer', 'archive-order', 'duplicate-path', 'path-traversal'):
        changed = json.loads(original_manifest)
        if label == 'manifest-missing':
            manifest_path.unlink()
        elif label == 'manifest-tamper':
            manifest_path.write_bytes(original_manifest + b'!')
        else:
            if label == 'archive-order':changed['archives'].reverse()
            elif label == 'duplicate-path':changed['files'].append(changed['files'][0])
            elif label == 'path-traversal':changed['files'][0]['path'] = '../escape.h'
            else:
                key,value = {'wrong-abi':('abi',2),'wrong-namespace':('namespace','other-runtime'),
                    'wrong-provider':('provider','Clang'),'wrong-version':('version','19.0.0'),
                    'wrong-target':('target','arm64-windows-msvc'),'wrong-crt':('crt','MD'),
                    'wrong-profile':('profile','Debug'),'wrong-sanitizer':('sanitizers',1)}[label]
                changed[key] = value
            encoded = (json.dumps(changed,indent=2)+'\n').encode()
            manifest_path.write_bytes(encoded)
            # A matching anchor still cannot turn a different tuple/order into this ABI.
            config_path.write_bytes(original_config.replace(hashlib.sha256(original_manifest).hexdigest().encode(),
                                                           hashlib.sha256(encoded).hexdigest().encode()))
        reject_config(label)
        manifest_path.write_bytes(original_manifest);config_path.write_bytes(original_config)
        negative.append(label)
    second = work/'second-prefix'
    shutil.copytree(moved, second)
    reject_config('mixed-target-prefix', extra=['-DTEST_SECOND_PREFIX=' + str(second)])
    negative.append('mixed-target-prefix')
    original_include = runtime/'include'
    saved_include = runtime/'include-original'
    indirect_include = work/'foreign-include'
    shutil.copytree(original_include, indirect_include)
    original_include.rename(saved_include)
    run('create-junction', ['cmd','/d','/c','mklink','/J',original_include,indirect_include])
    reject_config('junction-member')
    original_include.rmdir()
    saved_include.rename(original_include)
    negative.append('junction-member')
    for label, extra in [('global-configure-md',['-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL']),
                         ('global-configure-debug',['-DCMAKE_BUILD_TYPE=Debug']),
                         ('global-configure-fast',['-DCMAKE_C_FLAGS=/fp:fast']),
                         ('global-configure-asan',['-DCMAKE_C_FLAGS=/fsanitize=address']),
                         ('global-configure-clang',['-DCMAKE_C_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe',
                                             '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe'])]:
        reject_config(label, extra=extra)
        negative.append(label)
    inputs_path = args.build/'native-runtime-inputs.json'
    archive = Path(json.loads(inputs_path.read_text())['archives'][0]['file'])
    original_archive = archive.read_bytes()
    (work/'original-producer-archive.lib').write_bytes(original_archive)
    unknown = b'unknown.obj/'.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'100644'.ljust(8) + b'4'.ljust(10) + b'`\nBAD!'
    assert len(unknown) == 64
    try:
        archive.write_bytes(original_archive + unknown)
        rejected = run('unknown-producer-member', [os.sys.executable,'-B',args.source/'scripts/build_native_runtime_bundle.py',
            '--root',args.source,'--build',args.build,'--inputs',inputs_path,'--output',args.build/'native-runtime-bundle','--verify'], 'failure')
        assert b'Expected x64 COFF' in rejected.stderr
    finally:
        archive.write_bytes(original_archive)
    negative.append('unknown-producer-member')
    bundle = args.build/'native-runtime-bundle'
    verifier = [os.sys.executable,'-B',args.source/'scripts/build_native_runtime_bundle.py',
        '--root',args.source,'--build',args.build,'--inputs',inputs_path,'--output',bundle,'--verify']
    bundle_version = bundle/'XrayNativeRuntimeConfigVersion.cmake'
    original_version = bundle_version.read_bytes()
    for change in ('missing','tamper'):
        try:
            if change == 'missing':bundle_version.unlink()
            else:bundle_version.write_bytes(original_version + b'# changed\n')
            rejected = run('bundle-version-' + change, verifier, 'failure')
            assert b'package version identity mismatch' in rejected.stderr or b'Missing or indirect input' in rejected.stderr
        finally:
            bundle_version.write_bytes(original_version)
        negative.append('bundle-version-' + change)
    installed_version = package/'XrayNativeRuntimeConfigVersion.cmake'
    original_version = installed_version.read_bytes()
    for change in ('missing','tamper'):
        try:
            if change == 'missing':installed_version.unlink()
            else:installed_version.write_bytes(original_version + b'# changed\n')
            rejected = reject_config('installed-version-' + change, extra=['-DTEST_NO_VERSION=ON'])
            assert b'XrayNativeRuntime package version' in rejected.stderr
        finally:
            installed_version.write_bytes(original_version)
        negative.append('installed-version-' + change)
    extra = bundle/'include/unlisted-runtime.h'
    try:
        extra.write_bytes(b'/* Unlisted package member. */\n')
        rejected = run('bundle-unlisted-header', verifier, 'failure')
        assert b'Unlisted or missing bundle members' in rejected.stderr
        rejected_prefix = work/'rejected-extra-prefix'
        run('install-unlisted-header',['cmake','--install',args.build,'--component','XrayNativeRuntime','--prefix',rejected_prefix], 'failure')
        assert not list(rejected_prefix.rglob('XrayNativeRuntimeConfig.cmake'))
    finally:
        extra.unlink()
    negative.extend(['bundle-unlisted-header','install-unlisted-header'])
    for folder,name in ((runtime/'include','unlisted-runtime.h'),(package,'unlisted.cmake')):
        extra = folder/name
        before = config_path.read_bytes()
        try:
            extra.write_bytes(b'# unlisted\n')
            rejected = reject_config('installed-unlisted-' + name, extra=['-DTEST_NO_VERSION=ON'])
            assert b'XrayNativeRuntime unlisted' in rejected.stderr
            rejected = run('reinstall-unlisted-' + name,['cmake','--install',args.build,'--component','XrayNativeRuntime','--prefix',moved], 'failure')
            assert b'Native runtime destination has an unlisted' in rejected.stderr
            assert config_path.read_bytes() == before
        finally:
            extra.unlink()
        negative.extend(['installed-unlisted-' + name,'reinstall-unlisted-' + name])
    for label, options in [('md',['-DTEST_CRT_OVERRIDE=MultiThreadedDLL']),
                           ('debug',['-DTEST_EXTRA_OPTIONS=/D_DEBUG=1']),
                           ('asan',['-DTEST_EXTRA_OPTIONS=/fsanitize=address']),
                           ('fast',['-DTEST_EXTRA_OPTIONS=/fp:fast']),
                           ('arm64ec',['-DTEST_EXTRA_OPTIONS=/D_M_ARM64EC=1'])]:
        target = work/('override-' + label)
        configure('target-configure-' + label, moved, target, options)
        result = run('compile-' + label, ['cmake','--build',target,'--target','f64_public_c','-j2'], 'failure')
        assert b'XrayNativeRuntime requires' in result.stdout + result.stderr or b'XrayNativeRuntime does not distribute' in result.stdout + result.stderr
        assert not (target/'f64_public_c.exe').exists()
        negative.append(label)
    for relative, facts in source_inputs.items():
        assert hashlib.sha256((args.source/relative).read_bytes()).hexdigest() == facts['rawSHA256']
    summary = dict(state='PASS', sourceCommit=manifest['sourceCommit'], component='XrayNativeRuntime',
        installedPrefix=str(prefix), movedPrefix=str(moved), originalPrefixAbsent=not prefix.exists(),
        manifestSHA256=hashlib.sha256((runtime/'native_runtime_manifest.json').read_bytes()).hexdigest(),
        clients=cases, clientCount=len(cases), sourceInputs=source_inputs,
        negative=negative, arithmeticRows=3216, comparisonPairs=676,
        roundingModes=4, ftzDazStates=4, textRows=290, textSourceBytes=138, textOracleBytes=24,
        hello='NOT_RUN_IN_THIS_INSTALLED_MATRIX')
    (work/'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(work)


if __name__ == '__main__':
    main()
