"""Move an SDK-only install and build real public Source embedding consumers."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--consumers-only', action='store_true',
                        help='Run relocated public consumers without the producer/file rejection matrix')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build, parent = args.build.resolve(), args.output.resolve()
    assert parent.is_relative_to(build) and parent != build
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='source-sdk-', dir=parent))
    records = []
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')

    def run(label, command, success=True):
        result = subprocess.run(list(map(str, command)), cwd=output, env=environment,
                                capture_output=True, timeout=120)
        (output/(label+'.stdout')).write_bytes(result.stdout)
        (output/(label+'.stderr')).write_bytes(result.stderr)
        records.append(dict(label=label, argv=list(map(str, command)), exit_code=result.returncode))
        (output/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert (result.returncode == 0) == success, (label, result.returncode, result.stdout, result.stderr)
        return result.stdout

    original, moved = output/'original', output/'moved'
    run('install-sdk', ['cmake', '--install', build, '--component', 'XraySDK', '--prefix', original])
    assert original.resolve().is_relative_to(output) and moved.resolve().is_relative_to(output)
    original.rename(moved)
    assert not original.exists() and not (moved/'bin/xray.exe').exists()
    source = moved/'lib/xray/source-sdk'
    manifest = json.loads((source/'source_sdk_manifest.json').read_text(encoding='utf-8'))
    assert manifest['api_version'] == 4 and manifest['crt'] == 'MD' and manifest['sanitizers'] == 0
    assert len(manifest['archives']) == 15
    for row in manifest['files']:
        data = (source/row['path']).read_bytes()
        assert len(data) == row['length'] and hashlib.sha256(data).hexdigest() == row['sha256']
    stdlib = output/'empty-stdlib'
    stdlib.mkdir()
    consumer = output/'consumer.c'
    consumer.write_bytes((root/'tests/unit/xir/source_sdk_consumer.c').read_bytes())
    (output/'nominal_projection_fixture.h').write_bytes(
        (root/'tests/unit/xir/nominal_projection_fixture.h').read_bytes())
    (output/'source_ctfe_cleanup_cases.h').write_bytes(
        (root/'tests/unit/xir/source_ctfe_cleanup_cases.h').read_bytes())
    (output/'ctfe_cleanup').mkdir()
    (output/'ctfe_cleanup/cleanup_owned.xr').write_bytes(
        (root/'tests/unit/xir/ctfe_cleanup/cleanup_owned.xr').read_bytes())
    (output/'declarations_control_fixture.h').write_bytes(
        (root/'tests/unit/xir/declarations_control_fixture.h').read_bytes())
    (output/'initialization_cycle_fixture.h').write_bytes(
        (root/'tests/unit/xir/initialization_cycle_fixture.h').read_bytes())
    fixture = root/'tests/unit/xir/source_sdk_consumer'

    def configure(label, directory, success=True):
        run(label, ['cmake', '-S', fixture, '-B', directory, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER='+args.compiler,
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCMAKE_PREFIX_PATH='+str(moved),
            '-DCONSUMER_SOURCE='+str(consumer)], success)

    configure('consumer-configure', output/'consumer-build')
    run('consumer-build', ['cmake', '--build', output/'consumer-build', '--parallel', '2'])
    stdout = run('consumer-run', [output/'consumer-build/source_consumer.exe', stdlib])
    task_cases = (
        ('scalar-control', 11), ('array-i64', 6), ('nested-array-string', 16),
        ('nullable-i64', 4), ('nullable-array', 3), ('tuple-string', 9),
        ('enum-array', 6), ('atomic-shared', 6), ('task-nested', 6), ('try-optional', 58), ('typed-await', 55))
    expected = b''.join(f'case={name} PASS bytes={length} producers-dead\n'.encode('ascii')
                        for name, length in task_cases)
    expected += b'Installed public Source SDK: Checked io/time, detached cancellation and owned panic PASS\n'
    assert stdout.replace(b'\r\n', b'\n') == expected
    object_file, direct = output/'direct.obj', output/'direct.exe'
    if 'clang' in Path(args.compiler).name.lower() and 'clang-cl' not in Path(args.compiler).name.lower():
        compile_command = [args.compiler, '--target=x86_64-windows-msvc', '-std=c11', '-O2',
            '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fms-runtime-lib=dll',
            '-I'+str(source/'include'), '-c', str(consumer), '-o', str(object_file)]
    else:
        compile_command = [args.compiler, '/nologo', '/std:c11', '/utf-8', '/MD', '/O2', '/W4', '/WX',
            '/I'+str(source/'include'), '/c', str(consumer), '/Fo'+str(object_file)]
    run('direct-compile', compile_command)
    run('direct-link', ['link', '/nologo', '/incremental:no', '/out:'+str(direct), str(object_file),
        *[str(source/path) for path in manifest['archives']], 'kernel32.lib', '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES'])
    direct_stdout = run('direct-run', [direct, stdlib])
    assert direct_stdout == stdout
    cpp_source = output/'consumer.cpp'
    cpp_source.write_bytes((root/'tests/unit/xir/source_sdk_consumer.cpp').read_bytes())
    cpp_object, cpp_exe = output/'cpp.obj', output/'cpp.exe'
    if 'clang' in Path(args.compiler).name.lower() and 'clang-cl' not in Path(args.compiler).name.lower():
        cpp_command = [args.compiler, '--target=x86_64-windows-msvc', '-std=c++17', '-O2',
            '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fms-runtime-lib=dll',
            '-I'+str(source/'include'), '-c', str(cpp_source), '-o', str(cpp_object)]
    else:
        cpp_command = [args.compiler, '/nologo', '/std:c++17', '/utf-8', '/MD', '/O2', '/W4', '/WX', '/EHsc',
            '/I'+str(source/'include'), '/c', str(cpp_source), '/Fo'+str(cpp_object)]
    run('cpp-compile', cpp_command)
    run('cpp-link', ['link', '/nologo', '/incremental:no', '/out:'+str(cpp_exe), str(cpp_object),
        *[str(source/path) for path in manifest['archives']], 'kernel32.lib', '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES'])
    cpp_stdout = run('cpp-run', [cpp_exe, stdlib])
    assert cpp_stdout.replace(b'\r\n', b'\n') == b'Installed C++ public Source SDK: detached ownership and NUL output PASS\n'
    assert not list(stdlib.rglob('*.xr')) and not list(stdlib.rglob('*.chk'))
    if args.consumers_only:
        assert len(records) == 10
        summary = dict(status='PASS', scope='relocated-public-consumers', compiler=args.compiler,
            sdk_only=True, old_prefix_unavailable=True, source_files=0, checked_files=0,
            archives=len(manifest['archives']), actual_objects=sum(len(row['sources'])
                for row in json.loads((source/'source_sdk_recipe.json').read_text())['archives']),
            sdk_manifest_sha256=hashlib.sha256((source/'source_sdk_manifest.json').read_bytes()).hexdigest(),
            commands=len(records), not_run=['producer/file rejection matrix'], production_release=False)
        (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(summary))
        return
    attacks = []
    published = build/'source-sdk'
    publication_before = {path.relative_to(published).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in published.rglob('*') if path.is_file()}
    inputs = json.loads((build/'source-sdk-inputs.json').read_text(encoding='utf-8'))
    for case in ('missing-archive', 'unknown-object-target', 'duplicate-target'):
        invalid = json.loads(json.dumps(inputs))
        if case == 'missing-archive': invalid['archives'][-1]['file'] = str(build/'missing-source-archive.lib')
        elif case == 'unknown-object-target': invalid['archives'][-1]['target'] = 'missing-object-target'
        else: invalid['archives'].append(dict(invalid['archives'][-1]))
        input_file = output/('producer-'+case+'.json')
        input_file.write_text(json.dumps(invalid), encoding='utf-8')
        run('producer-'+case, [sys.executable, '-B', root/'scripts/build_source_sdk.py',
            '--root', root, '--build', build, '--inputs', input_file, '--output', published,
            '--provider', manifest['provider'], '--profile', manifest['profile']], False)
        after = {path.relative_to(published).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in published.rglob('*') if path.is_file()}
        assert after == publication_before
        attacks.append('producer-'+case+'-publication-preserved')
    members = ['source_sdk_manifest.json', *[row['path'] for row in manifest['files']]]
    assert len(members) == len(set(members))
    for index, relative in enumerate(members):
        member = source/relative
        before = member.read_bytes()
        try:
            member.unlink()
            configure('missing-'+str(index), output/('missing-'+str(index)), False)
            attacks.append('missing-'+relative)
            changed = bytearray(before); changed[0] ^= 1
            member.write_bytes(changed)
            configure('changed-'+str(index), output/('changed-'+str(index)), False)
            attacks.append('changed-'+relative)
        finally:
            member.write_bytes(before)
    summary = dict(status='PASS', compiler=args.compiler, sdk_only=True, old_prefix_unavailable=True,
        source_files=0, checked_files=0, archives=len(manifest['archives']), actual_objects=sum(len(row['sources'])
            for row in json.loads((source/'source_sdk_recipe.json').read_text())['archives']),
        sdk_manifest_sha256=hashlib.sha256((source/'source_sdk_manifest.json').read_bytes()).hexdigest(),
        attacks=attacks, commands=len(records), production_release=False)
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
