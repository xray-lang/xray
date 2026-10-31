"""Consume real generated TaskResult values from a moved current runtime SDK."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--program', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
    environment.pop('XRAY_STDLIB_PATH', None)
    original, prefix = out/'original', out/'moved'
    manifest = json.loads((args.bundle/'sdk_manifest.json').read_text(encoding='utf-8'))
    assert tuple(manifest[key] for key in ('wire', 'semantic', 'value_abi', 'call_abi', 'program_abi')) == (39, 109, 30, 37, 37)
    assert manifest['crt'] == 2 and manifest['sanitizers'] == 0
    for row in manifest['files']:
        source, destination = args.bundle/row['path'], original/row['path']
        raw = source.read_bytes()
        assert len(raw) == row['length'] and hashlib.sha256(raw).hexdigest() == row['sha256'], row['path']
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
    original.rename(prefix)
    assert not original.exists() and not (prefix/'bin/xray.exe').exists()
    customer = out/'customer'
    customer.mkdir()
    for name in ('test_task_result_native.c', 'test_task_result_cpp.cpp',
                 'task_result_bridge.h', 'fixtures/task_result.expected.json'):
        shutil.copyfile(here/name, customer/Path(name).name)
    shutil.copyfile(args.program, customer/'program.c')
    records = []

    def run(name, command, expected=None):
        command = list(map(str, command))
        stdout, stderr = out/(name+'.stdout'), out/(name+'.stderr')
        with stdout.open('wb') as output, stderr.open('wb') as error:
            status = run_child(command, cwd=customer, env=environment,
                               stdout=output, stderr=error, timeout=240)
        records.append(dict(name=name, command=command, exit=status))
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert not status, (name, status, stdout.read_bytes(), stderr.read_bytes())
        if expected is not None:
            assert stdout.read_bytes() == expected and not stderr.read_bytes(), name

    includes = ['/I'+str(prefix/path) for path in ('src', 'include', 'generated')]
    options = ['/nologo', '/MD', '/O2', '/W4', '/WX', '/utf-8', '/DNDEBUG', '/DNOMINMAX',
               '/DWIN32_LEAN_AND_MEAN', '/D_CRT_SECURE_NO_WARNINGS', *includes]
    run('program-c11', [args.compiler, *options, '/std:c11', '/experimental:c11atomics',
                       '/c', 'program.c', '/Foprogram.obj'])
    run('client-c11', [args.compiler, *options, '/std:c11', '/experimental:c11atomics',
                      '/c', 'test_task_result_native.c', '/Foclient.obj'])
    run('bridge-c11', [args.compiler, *options, '/std:c11', '/experimental:c11atomics',
                      '/DXR_TASK_RESULT_BRIDGE=1', '/c', 'test_task_result_native.c', '/Fobridge.obj'])
    run('client-cpp17', [args.compiler, *options, '/std:c++17', '/EHsc',
                        '/c', 'test_task_result_cpp.cpp', '/Focpp.obj'])
    libraries = [prefix/row['path'] for row in manifest['files'] if row['kind'] == 5]
    assert len(libraries) == 5
    system = [name+'.lib' for name in manifest['system_libraries']]
    linker = args.compiler.with_name('link.exe')
    run('link-c11', [linker, '/nologo', '/incremental:no', '/out:c11.exe', 'client.obj', 'program.obj', *libraries, *system])
    run('link-cpp17', [linker, '/nologo', '/incremental:no', '/out:cpp17.exe', 'cpp.obj', 'bridge.obj', 'program.obj', *libraries, *system])
    entries = ['successResult', 'errorResult', 'panicResult', 'cancelledResult', customer/'task_result.expected.json']
    run('run-c11', [customer/'c11.exe', *entries],
        b'TaskResult generated C: four owned variants, exact full output and retired producers PASS\r\n')
    run('run-cpp17', [customer/'cpp17.exe', *entries],
        b'C++17 TaskResult: new header, type_info and detached four-state ownership PASS\r\n')
    result = dict(state='PASS', semantic=109, callABI=37, source='real generated C11 Program',
                  programSHA256=hashlib.sha256((customer/'program.c').read_bytes()).hexdigest(),
                  commandCount=len(records), originalPrefixAbsent=True,
                  producerRetiredBeforeRead=True, cppBoundary='TaskResult/value headers through a C11 Program bridge')
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
