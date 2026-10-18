"""Exercise installed public Source headers with C11 and C++20 clients."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile


EXPECTED = (b'Installed public REPL: typed history, busy callback, cancel/defer, resume and owned survivors PASS\n'
            b'Installed public REPL inspection: visible bindings, nonpublishing types and owned survivors PASS\n'
            b'Installed Source SDK smoke: AST, typed values, persistent state and REPL PASS\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--dumpbin', required=True)
    parser.add_argument('--asan-runtime', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build, parent = args.build.resolve(), args.output.resolve()
    assert parent.is_relative_to(build) and parent != build
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='SDK 客户-', dir=parent)).resolve()
    env = dict(os.environ, TEMP=str(out), TMP=str(out), PYTHONDONTWRITEBYTECODE='1',
               PYTHONIOENCODING='utf-8')
    if args.asan_runtime:
        assert (args.asan_runtime/'clang_rt.asan_dynamic-x86_64.dll').is_file()
        env['PATH'] = str(args.asan_runtime) + os.pathsep + env.get('PATH', '')
        env['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1'
    records = []

    def run(label, command, okay=True):
        result = subprocess.run(list(map(str, command)), cwd=out, env=env,
                                capture_output=True, timeout=120)
        (out/(label+'.stdout')).write_bytes(result.stdout)
        (out/(label+'.stderr')).write_bytes(result.stderr)
        records.append(dict(label=label, argv=list(map(str, command)), exit_code=result.returncode))
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert (result.returncode == 0) == okay, (label, result.returncode, result.stdout, result.stderr)
        return result.stdout

    original, moved = out/'original', out/'moved'
    run('install', ['cmake', '--install', build, '--component', 'XraySDK', '--prefix', original])
    assert original.resolve().is_relative_to(out) and not moved.exists()
    original.rename(moved)
    sdk = moved/'lib/xray/source-sdk'
    manifest = json.loads((sdk/'source_sdk_manifest.json').read_text(encoding='utf-8'))
    assert manifest['crt'] == 'MD' and manifest['sanitizers'] == 0
    headers = {'xray_source.h', 'xray_export.h', 'xray_source_ast.h', 'xray_source_entry.h',
               'xray_source_instance.h', 'xray_source_repl.h', 'xray_source_value.h'}
    assert {Path(r['path']).name for r in manifest['files'] if r['path'].startswith('include/')} == headers
    for row in manifest['files']:
        data = (sdk/row['path']).read_bytes()
        assert len(data) == row['length'] and hashlib.sha256(data).hexdigest() == row['sha256']
    assert not (moved/'bin/xray.exe').exists()
    assert not any(p.suffix in ('.xr', '.chk') for p in moved.rglob('*'))
    payload = json.loads((moved/'share/xray/install/payload-manifest.json').read_text(encoding='utf-8'))
    assert payload['components'] == ['sdk']
    names = set()
    for name in headers - {'xray_export.h'}:
        text = (sdk/'include'/name).read_text(encoding='utf-8')
        names.update(re.findall(r'XRAY_API\s+[^;]*?\b(xray_\w+)\s*\(', text, re.S))
    exports = run('public-exports', [args.dumpbin, '/nologo', '/linkermember:1', sdk/'lib/xray_source_api.lib'])
    assert len(names) == 67
    observed = set(re.findall(r'\b(xray_source_\w+)\b', exports.decode('utf-8')))
    assert names <= observed
    customer = out/'customer'
    customer.mkdir()
    for language in ['c', 'cpp']:
        shutil.copyfile(root/'tests/unit/xir/source_sdk_smoke_consumer.c', customer/('client.'+language))
    project = '''cmake_minimum_required(VERSION 3.25)
project(InstalledSourceSmoke C CXX)
find_package(XraySource CONFIG REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
foreach(language c cpp)
    add_executable(source_${language} client.${language})
    target_link_libraries(source_${language} PRIVATE Xray::Source)
    set_target_properties(source_${language} PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(source_${language} PRIVATE /utf-8 /W4 /WX)
    if(SOURCE_CLIENT_ASAN)
        target_compile_options(source_${language} PRIVATE /fsanitize=address /Oy-)
        target_link_libraries(source_${language} PRIVATE
            "${SOURCE_ASAN_RUNTIME}/clang_rt.asan_dynamic-x86_64.lib")
        target_link_options(source_${language} PRIVATE
            "/wholearchive:${SOURCE_ASAN_RUNTIME}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    endif()
endforeach()
'''
    (customer/'CMakeLists.txt').write_text(project, encoding='utf-8')
    customer_build = out/'customer-build'
    configure = ['cmake', '-S', customer, '-B', customer_build, '-G', 'Ninja',
                 '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER='+args.compiler,
                 '-DCMAKE_CXX_COMPILER='+args.compiler, '-DCMAKE_PREFIX_PATH='+str(moved)]
    if args.asan_runtime:
        configure += ['-DSOURCE_CLIENT_ASAN=ON', '-DSOURCE_ASAN_RUNTIME='+str(args.asan_runtime)]
    run('configure', configure)
    run('build', ['cmake', '--build', customer_build, '--parallel', '2'])
    commands = json.loads((customer_build/'compile_commands.json').read_text(encoding='utf-8'))
    assert len(commands) == 2
    for row in commands:
        text = row['command'].replace('\\', '/').lower()
        assert str(root).replace('\\', '/').lower() not in text
        assert Path(row['file']).resolve().is_relative_to(customer)
        assert ('fsanitize=address' in text) == bool(args.asan_runtime)
    dependencies = run('header-dependencies', ['ninja', '-C', customer_build, '-t', 'deps'])
    assert len(re.findall(rb'#deps ([1-9][0-9]*)', dependencies)) == 2
    assert str(root).replace('\\', '/').lower().encode() not in dependencies.replace(b'\\', b'/').lower()
    empty = out/'empty-stdlib'
    empty.mkdir()
    for language in ['c', 'cpp']:
        exe = customer_build/('source_'+language+'.exe')
        imports = run(language+'-imports', [args.dumpbin, '/dependents', exe])
        assert not re.search(rb'^\s*xray[^\s]*\.dll\s*$', imports, re.M | re.I)
        assert (b'clang_rt.asan_dynamic-x86_64.dll' in imports.lower()) == bool(args.asan_runtime)
        assert run(language+'-run', [exe, empty, empty, empty]).replace(b'\r\n', b'\n') == EXPECTED
    hidden = out/'unavailable-sdk'
    assert moved.resolve().is_relative_to(out) and not hidden.exists()
    moved.rename(hidden)
    for language in ['c', 'cpp']:
        assert run(language+'-run-without-sdk', [customer_build/('source_'+language+'.exe'), empty, empty, empty]).replace(b'\r\n', b'\n') == EXPECTED
    summary = dict(status='PASS', languages=['C11', 'C++20'], compiler=args.compiler,
        customer_ASAN=bool(args.asan_runtime), shipping_sdk_sanitizers=0,
        full_internal_SDK_ASAN_qualification=False, public_symbols=67, headers=sorted(headers),
        no_source_checkout_dependency=True, SDK_path_hidden_after_link=True,
        original_AST_values_Instance_REPL_and_producer_death_assertions=True,
        source_manifest_sha256=hashlib.sha256((hidden/'lib/xray/source-sdk/source_sdk_manifest.json').read_bytes()).hexdigest(),
        commands=len(records), production_release=False)
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
