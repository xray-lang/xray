"""Exercise owning public API through moved SDK-only files and real C/C++ hosts."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--secondary-compiler')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build, parent = args.build.resolve(), args.output.resolve()
    assert parent.is_relative_to(build) and parent != build
    parent.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='owned-sdk-', dir=parent))
    records = []
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    environment.pop('XRAY_STDLIB_PATH', None)

    def run(name, argv, okay=True):
        result = subprocess.run(list(map(str, argv)), cwd=out, env=environment,
                                capture_output=True, timeout=120)
        (out/(name+'.stdout')).write_bytes(result.stdout)
        (out/(name+'.stderr')).write_bytes(result.stderr)
        records.append(dict(name=name, argv=list(map(str, argv)), exit_code=result.returncode))
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n', encoding='utf-8')
        assert (result.returncode == 0) == okay, (name, result.returncode, result.stdout, result.stderr)
        return result.stdout

    original, moved = out/'original', out/'moved'
    run('install', ['cmake', '--install', build, '--component', 'XraySDK', '--prefix', original])
    assert original.resolve().is_relative_to(out) and moved.resolve().is_relative_to(out) and not moved.exists()
    original.rename(moved)
    sdk = moved/'lib/xray/source-sdk'
    manifest = json.loads((sdk/'source_sdk_manifest.json').read_text(encoding='utf-8'))
    headers = {'xray_source.h', 'xray_export.h', 'xray_source_ast.h',
               'xray_source_value.h', 'xray_source_entry.h', 'xray_source_instance.h', 'xray_source_repl.h'}
    assert manifest['api_version'] == 2 and manifest['crt'] == 'MD' and manifest['sanitizers'] == 0
    assert len(manifest['archives']) == 15
    assert {Path(r['path']).name for r in manifest['files'] if r['path'].startswith('include/')} == headers
    for row in manifest['files']:
        data = (sdk/row['path']).read_bytes()
        assert len(data) == row['length'] and hashlib.sha256(data).hexdigest() == row['sha256']
    assert not (moved/'bin/xray.exe').exists() and not any(p.suffix == '.xr' for p in moved.rglob('*'))
    source = out/'consumer.c'
    source.write_bytes((root/'tests/unit/xir/source_sdk_owned_consumer.c').read_bytes())
    fragment = 'source_sdk_repl_input_consumer.inc.c'
    (out/fragment).write_bytes((root/'tests/unit/xir'/fragment).read_bytes())
    fragment = 'source_sdk_repl_array_consumer.inc.c'
    (out/fragment).write_bytes((root/'tests/unit/xir'/fragment).read_bytes())
    fragment = 'source_sdk_repl_write_consumer.inc.c'
    (out/fragment).write_bytes((root/'tests/unit/xir'/fragment).read_bytes())
    fragment = 'source_sdk_repl_hosted_consumer.inc.c'
    (out/fragment).write_bytes((root/'tests/unit/xir'/fragment).read_bytes())
    fragment = 'source_sdk_task_result_consumer.inc.c'
    (out/fragment).write_bytes((root/'tests/unit/xir'/fragment).read_bytes())
    fixtures = out/'fixtures'
    fixtures.mkdir()
    for name in ('source_public_repl_rooted', 'source_public_repl_file'):
        original_fixture = root/'tests/unit/api'/name
        shutil.copytree(original_fixture, fixtures/name)
        for path in original_fixture.rglob('*'):
            if path.is_file():
                assert path.read_bytes() == (fixtures/name/path.relative_to(original_fixture)).read_bytes()
    empty = out/'empty-stdlib'
    empty.mkdir()
    compilers = [Path(shutil.which(args.compiler) or args.compiler).resolve()]
    if args.secondary_compiler:
        secondary = Path(shutil.which(args.secondary_compiler) or args.secondary_compiler).resolve()
        if not secondary.samefile(compilers[0]):
            compilers.append(secondary)
    expected = (b'Installed public REPL: typed history, busy callback, cancel/defer, resume and owned survivors PASS\n'
                b'Installed public REPL inspection: visible bindings, nonpublishing types and owned survivors PASS\n'
                b'Installed rooted REPL: real imports, repeated path births, old closure, type side effects and no fallback PASS\n'
                b'Installed FILE REPL: real imports, repeated births, absent/directory/config refusals and owned survivors PASS\n'
                b'Installed input and snapshot: exact lexer span, pending exclusion, removed file and producer death PASS\n'
                b'Installed Array history: exact birth push, argument once, const refusal, panic effect, shadow and owned snapshot PASS\n'
                b'Installed Class history control: independent later field read PASS\n'
                b'Installed typed binding writes: birth authority, scalar/String/Unit/Array/struct/Class, foreign refusal, busy and owned survivor PASS\n'
                b'Installed hosted lifecycle: explicit owners, child and root yields, busy drop, defaults reset and owned survivors PASS\n'
                b'Installed owning AST, typed values, persistent state, callable authority and producer death PASS\n'
                b'Installed terminal Task outcomes: repeated owned data, nested Task42, thrown and NUL/UTF8 panic after all producers PASS\n')
    for index, compiler in enumerate(compilers):
        for language in ('c', 'cpp'):
            label = f'provider{index}-{language}'
            obj, exe = out/(label+'.obj'), out/(label+'.exe')
            if 'clang' in compiler.name.lower() and 'clang-cl' not in compiler.name.lower():
                command = [compiler, '--target=x86_64-windows-msvc', '-x', 'c++' if language == 'cpp' else 'c',
                           '-std=c++20' if language == 'cpp' else '-std=c11', '-O2', '-Wall', '-Wextra',
                           '-Werror', '-pedantic-errors', '-fms-runtime-lib=dll', '-I'+str(sdk/'include'),
                           '-c', source, '-o', obj]
            else:
                command = [compiler, '/nologo', '/TP' if language == 'cpp' else '/TC',
                           '/std:c++20' if language == 'cpp' else '/std:c11', '/utf-8', '/MD', '/O2',
                           '/W4', '/WX', '/I'+str(sdk/'include'), '/c', source, '/Fo'+str(obj)]
            run(label+'-compile', command)
            run(label+'-link', ['link', '/nologo', '/incremental:no', '/out:'+str(exe), obj,
                              *[sdk/p for p in manifest['archives']], 'kernel32.lib',
                              '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES'])
            snapshot = out/'installed-owned-snapshot.xr'
            with snapshot.open('xb') as stream:
                stream.write(b'const snapshot=72\nsnapshot\n')
            assert snapshot.read_bytes() == b'const snapshot=72\nsnapshot\n'
            assert run(label+'-run', [exe, empty, fixtures/'source_public_repl_rooted',
                                     fixtures/'source_public_repl_file']).replace(b'\r\n', b'\n') == expected
            assert not (out/'installed-owned-snapshot.xr').exists()
    probe = out/'config-probe'
    probe.mkdir()
    (probe/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\n'
        'project(OwnedSdkIntegrity C)\nfind_package(XraySource CONFIG REQUIRED)\n', encoding='utf-8')
    attacks = []
    for index, name in enumerate(sorted(headers - {'xray_source.h', 'xray_export.h'})):
        path = sdk/'include'/name
        saved = path.read_bytes()
        for mode in ('missing', 'changed'):
            if mode == 'missing':
                path.unlink()
            else:
                changed = bytearray(saved)
                changed[len(changed)//2] ^= 1
                path.write_bytes(changed)
            label = f'header-{index}-{mode}'
            run(label, ['cmake', '-S', probe, '-B', out/label, '-G', 'Ninja',
                        '-DCMAKE_C_COMPILER='+str(compilers[0]), '-DCMAKE_PREFIX_PATH='+str(moved)], False)
            path.write_bytes(saved)
            attacks.append(label)
    summary = dict(status='PASS', sdk_only=True, providers=list(map(str, compilers)), languages=['C11', 'C++20'],
                   published_headers=sorted(headers), exact_NUL_UTF8=True, independent_persistent_instances=True,
                   foreign_callable_refused=True, all_producers_dead=True, owned_tuple_child=True, attacks=attacks,
                   public_REPL_functions=14, borrowed_binding_facts=True, type_query_actual_side_effects=True,
                   type_query_preserves_it=True, type_result_survives_owner=True,
                   rooted_real_imports_and_project_authority=True, repeated_real_path_births=True,
                   FILE_snapshot_after_unlink=True, real_lexer_exact_span=True,
                   actual_array_birth_mutation=True, array_argument_evaluated_once=True,
                   array_const_refused=True, failed_input_old_array_effect_preserved=True,
                   old_array_snapshot_survives_shadow_and_owner_death=True,
                   independent_class_history_control=True, actual_typed_birth_writes=True,
                   write_preserves_bindings_and_it=True, write_pending_and_callback_BUSY=True,
                   write_foreign_composite_refused_after_producer_death=True,
                   written_NUL_UTF8_survives_all_producers=True,
                   simultaneous_REPL_owners=True, child_then_root_yield=True,
                   second_owner_dies_before_first_resumes=True,
                   callback_drop_BUSY_preserves_pointer_and_event=True,
                   call_options_reinit_and_bad_size_preserve_outputs=True,
                   owned_results_survive_both_owners=True,
                   owned_terminal_Task_results_after_producer_death=True,
                   terminal_Task_nested42_typed_error_and_exact_NUL_UTF8_panic=True,
                   terminal_Task_repeated_copies_and_occupied_output_refusal=True,
                   manifest_sha256=hashlib.sha256((sdk/'source_sdk_manifest.json').read_bytes()).hexdigest(),
                   scope='Source embedding host consumers; not Target AOT/mixed or complete FI/safety qualification',
                   production_release=False)
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
