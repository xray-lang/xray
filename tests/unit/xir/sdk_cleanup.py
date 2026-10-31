"""Run moved-prefix C11/C++17 clients of the current cleanup ABI and Source API."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONUTF8='1')
    environment.pop('XRAY_STDLIB_PATH', None)
    records = []

    def run(name, command):
        command = list(map(str, command))
        executed = command
        if command[0] == 'cmake' and '--build' in command:
            batch = out/(name+'.cmd')
            batch.write_text('@echo off\nchcp 65001 >nul\n'+subprocess.list2cmdline(command)+
                             '\nexit /b %errorlevel%\n', encoding='utf-8', newline='\r\n')
            executed = ['cmd.exe', '/d', '/c', str(batch)]
        stdout, stderr = out/(name+'.stdout'), out/(name+'.stderr')
        with stdout.open('wb') as output, stderr.open('wb') as error:
            status = run_child(executed, cwd=out, env=environment, stdout=output, stderr=error, timeout=600)
        records.append(dict(name=name, command=command, executed=executed, exit=status))
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert not status, (name, status, stdout.read_bytes(), stderr.read_bytes())
        return stdout.read_bytes(), stderr.read_bytes()

    original, prefix = out/'original', out/'moved'
    run('install', ['cmake', '--install', args.build.resolve(), '--component', 'XraySDK', '--prefix', original])
    original.rename(prefix)
    assert not original.exists() and not (prefix/'bin/xray.exe').exists()
    runtime, source = prefix/'lib/xray/xir-runtime-sdk', prefix/'lib/xray/source-sdk'
    runtime_manifest = json.loads((runtime/'sdk_manifest.json').read_text(encoding='utf-8'))
    source_manifest = json.loads((source/'source_sdk_manifest.json').read_text(encoding='utf-8'))
    assert (runtime_manifest['call_abi'], runtime_manifest['semantic'], runtime_manifest['value_abi'], runtime_manifest['program_abi']) == (37, 108, 30, 37)
    assert runtime_manifest['crt'] == 2 and runtime_manifest['sanitizers'] == 0
    assert source_manifest['crt'] == 'MD' and source_manifest['sanitizers'] == 0
    for directory, manifest in ((runtime, runtime_manifest), (source, source_manifest)):
        for row in manifest['files']:
            content = (directory/row['path']).read_bytes()
            assert len(content) == row['length'] and hashlib.sha256(content).hexdigest() == row['sha256'], row['path']
    customer = out/'customer'
    customer.mkdir()
    client = root/'tests/unit/xir/sdk_cleanup_consumer.c'
    shutil.copyfile(client, customer/'client.c')
    shutil.copyfile(client, customer/'client.cpp')
    fixture = customer/'input.xr'
    shutil.copyfile(root/'tests/unit/xir/p11_ctfe_reference/fixtures/cleanup_replaced_owned.xr', fixture)
    shutil.copytree(root/'stdlib', customer/'stdlib')
    (customer/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.25)
project(InstalledCleanup C CXX)
find_package(XraySource CONFIG REQUIRED)
foreach(language c cpp)
  add_executable(source_${language} client.${language})
  target_link_libraries(source_${language} PRIVATE Xray::Source)
  target_compile_definitions(source_${language} PRIVATE XR_CLEANUP_SOURCE_SDK=1)
  target_compile_options(source_${language} PRIVATE /W4 /WX /utf-8)
  set_target_properties(source_${language} PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
endforeach()
''', encoding='utf-8')
    run('configure-source', ['cmake', '-G', 'Ninja', '-S', customer, '-B', customer/'build',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_PREFIX_PATH='+str(prefix), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCMAKE_C_COMPILER='+args.compiler, '-DCMAKE_CXX_COMPILER='+args.compiler])
    run('build-source', ['cmake', '--build', customer/'build', '-j2'])
    expected = b'Installed Source cleanup: dual owned Error, fatal prior and retired producers PASS\r\n'
    for language in ('c', 'cpp'):
        stdout, stderr = run('source-'+language, [customer/'build'/('source_'+language+'.exe'), fixture, customer/'stdlib'])
        assert stdout == expected and not stderr, (language, stdout, stderr)
    libraries = [runtime/row['path'] for row in runtime_manifest['files'] if row['path'].endswith('.lib')]
    assert len(libraries) == 5
    for language in ('c', 'cpp'):
        obj, exe = customer/('runtime-'+language+'.obj'), customer/('runtime-'+language+'.exe')
        command = [args.compiler, '/nologo', '/MD', '/O2', '/W4', '/WX', '/utf-8', '/DNDEBUG', '/D_CRT_SECURE_NO_WARNINGS',
            '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/std:c11' if language == 'c' else '/std:c++17']
        if language == 'c':
            command.append('/experimental:c11atomics')
        else:
            command.append('/EHsc')
        for directory in ('src', 'include', 'generated'):
            command.append('/I'+str(runtime/directory))
        command += ['/c', str(customer/('client.'+language)), '/Fo'+str(obj)]
        run('compile-runtime-'+language, command)
        run('link-runtime-'+language, ['link', '/nologo', '/incremental:no', '/out:'+str(exe), obj, *libraries,
            *(name if name.endswith('.lib') else name+'.lib' for name in runtime_manifest['system_libraries']),
            '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES'])
        stdout, stderr = run('runtime-'+language, [exe])
        assert stdout == b'Installed Runtime Call37: owned fatal, moved result, retired Domain and old ABI refusal PASS\r\n' and not stderr
    summary = dict(state='PASS', source_api=source_manifest['api_version'], call_abi=37, common=runtime_manifest['semantic'],
        crt='MD', sanitizers=0, source_languages=['C11', 'C++17'], runtime_languages=['C11', 'C++17'], clients=4, prefix_moved=True,
        runtime_manifest_sha256=hashlib.sha256((runtime/'sdk_manifest.json').read_bytes()).hexdigest(),
        source_manifest_sha256=hashlib.sha256((source/'source_sdk_manifest.json').read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
        boundary='Targeted installed clients only; no full Source61 or sanitizer installation qualification.')
    (out/'result.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print('Current moved-prefix Source/Runtime cleanup C11 and C++17 clients PASS')


if __name__ == '__main__':
    main()
