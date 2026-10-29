"""Exercise runtime SDK structural refresh with the real CMake producer and Ninja."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    sys.path.insert(0, str(root/'scripts'))
    from xray_process import run_child
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='sdk-recipe-', dir=args.output))
    source, build = output/'source with spaces', output/'build with spaces'
    source.mkdir()
    for relative in ('scripts', 'src/base', 'src/xir', 'src/execution', 'src/shared'):
        (source/relative).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root/'scripts/gen_xir_runtime_sdk_recipe.py', source/'scripts/gen_xir_runtime_sdk_recipe.py')
    base = source/'src/base/base.c'
    base.write_text('int payload(void){return 1;}\n')
    (source/'src/base/time.c').write_text('int clock_stub(void){return 0;}\n')
    (source/'src/base/CMakeLists.txt').write_text('add_library(xray_compile_resources STATIC base.c)\n'
        'if(WIN32)\n target_sources(xray_compile_resources PRIVATE time.c)\nendif()\n')
    owner = source/'src/xir/CMakeLists.txt'
    owner.write_text('add_library(xray_xir_admission STATIC admission.c)\n'
        'add_library(xray_xir_declarations STATIC declarations.c)\n'
        'add_library(xray_xir_scalar STATIC scalar.c)\n')
    for name in ('admission', 'declarations', 'scalar'):
        (source/f'src/xir/{name}.c').write_text(f'int {name}(void){{return 0;}}\n')
    (source/'src/execution/xir_runtime_host.cmake').write_text('add_library(xray_xir_runtime_host STATIC host.c)\n')
    (source/'src/execution/host.c').write_text('int host(void){return 0;}\n')
    import importlib.util
    spec = importlib.util.spec_from_file_location('runtime_recipe', root/'scripts/gen_xir_runtime_sdk_recipe.py')
    generator = importlib.util.module_from_spec(spec);spec.loader.exec_module(generator)
    for relative in generator.GENERATED_RUNTIME_ROOTS:
        path = source/relative;path.parent.mkdir(parents=True, exist_ok=True);path.write_text('/* fixture leaf */\n')
    (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.21)\n'
        'project(RuntimeRecipeIncremental LANGUAGES C)\n'
        'file(APPEND "${CMAKE_BINARY_DIR}/configures.txt" "configure\\n")\n'
        f'set(XRAY_PYTHON "{Path(sys.executable).as_posix()}")\n'
        f'add_subdirectory("{(root/"src/toolchain").as_posix()}" toolchain)\n'
        'add_library(recipe_payload STATIC src/base/base.c)\n')
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', NO_COLOR='1')
    records = []

    def run(label, command, success=True):
        argv = list(map(str, command))
        if os.name == 'nt':
            batch = output/(label+'.cmd')
            batch.write_text('@echo off\nchcp 65001 >nul\n'+subprocess.list2cmdline(argv)+'\nexit /b %errorlevel%\n', newline='\r\n')
            actual = ['cmd.exe', '/d', '/c', str(batch)]
        else:
            actual = argv
        with (output/(label+'.stdout')).open('wb') as stdout, (output/(label+'.stderr')).open('wb') as stderr:
            code = run_child(actual, cwd=root, env=environment, stdout=stdout, stderr=stderr, timeout=90)
        records.append({'label': label, 'command': argv, 'exitCode': code})
        (output/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
        assert (code == 0) == success, (label, code, (output/(label+'.stdout')).read_bytes(), (output/(label+'.stderr')).read_bytes())
        return (output/(label+'.stdout')).read_text(encoding='utf-8', errors='replace')

    def configure_count():
        return len((build/'configures.txt').read_text().splitlines())

    def make(label, configured, success=True):
        log = run(label, ['cmake', '--build', build, '--target', 'recipe_payload', '-j2'], success)
        assert configure_count() == configured, (label, configure_count(), configured, log)
        return log

    run('configure', ['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_C_COMPILER='+args.compiler])
    make('initial', 1)
    recipe = build/'xir-runtime-sdk/sdk_recipe.json'
    before = recipe.read_bytes();initial_time = recipe.stat().st_mtime_ns
    base.write_text('int payload(void){return 2;}\n')
    log = make('body-only', 1)
    assert 'base.c' in log and recipe.read_bytes() == before and recipe.stat().st_mtime_ns == initial_time
    (source/'src/base/detail.h').write_text('#define DETAIL 3\n')
    base.write_text('#include "detail.h"\nint payload(void){return DETAIL;}\n')
    make('include-add', 2)
    assert 'src/base/detail.h' in recipe.read_text()
    structure = recipe.read_bytes();struct_time = recipe.stat().st_mtime_ns
    (source/'src/base/detail.h').write_text('#define DETAIL 4\n')
    log = make('header-body-only', 2)
    assert 'base.c' in log and recipe.read_bytes() == structure and recipe.stat().st_mtime_ns == struct_time
    base.write_text('int payload(void){return 2;}\n');(source/'src/base/detail.h').unlink()
    make('include-remove-and-delete', 3)
    assert 'src/base/detail.h' not in recipe.read_text()
    before = recipe.read_bytes()
    base.write_text('#include "missing.h"\nint payload(void){return 2;}\n')
    log = make('missing-include', 3, False)
    assert 'missing quoted dependency' in log and recipe.read_bytes() == before
    base.write_text('int payload(void){return 2;}\n');make('retry', 3)
    (source/'src/xir/extra.c').write_text('int extra(void){return 0;}\n')
    owner.write_text(owner.read_text().replace('STATIC admission.c)', 'STATIC admission.c extra.c)'))
    make('owner-add', 4)
    assert 'src/xir/extra.c' in recipe.read_text()
    owner.write_text(owner.read_text().replace('admission.c extra.c)', 'admission.c)'));(source/'src/xir/extra.c').unlink()
    make('owner-remove-and-delete', 5)
    assert 'src/xir/extra.c' not in recipe.read_text()
    (source/'.git').mkdir();(source/'.git/HEAD').write_text('fixture identity only\n')
    make('unrelated-file', 5)
    result = {'state': 'PASS', 'scope': 'Real production generator and CMake producer; incremental correctness, not a performance qualification',
              'commands': len(records), 'configures': 5, 'source': str(source), 'build': str(build),
              'recipeSHA256': hashlib.sha256(recipe.read_bytes()).hexdigest()}
    (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(result, evidence=str(output))))


if __name__ == '__main__':
    main()
