"""Link preserved old objects independently against the actual current SDK."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess

# These interfaces keep their original signatures and semantics. The remaining
# objects in the fixed old input set refer to retired compiler ownership APIs.
RETAINED = set('''
xr_xir_op_name xr_xir_module_imports xr_xir_type_in_context xr_xir_type_node
xr_xir_callable_signature xr_xir_type_is_callable xr_xir_type_is_array
xr_xir_type_is_nullable xr_xir_nullable_element xr_xir_type_is_cell
xr_xir_type_is_nominal xr_xir_type_is_struct xr_xir_type_is_enum xr_xir_type_is_class
xr_xir_type_is_owned xr_xir_cell_element xr_xir_array_element xr_xir_type_span xr_xir_operand_type
xr_xir_instance_config_init xr_xir_instance_weaken_function xr_xir_instance_new
xr_xir_instance_state xr_xir_instance_start xr_xir_instance_resume
xr_xir_instance_take_result xr_xir_instance_copy_failure xr_xir_instance_stop xr_xir_instance_free
xr_xir_instance_start_function xr_xir_instance_function xr_xir_instance_resolve_function
xr_xir_instance_literal xr_xir_instance_atomic xr_xir_instance_cell xr_xir_instance_cell_read
xr_xir_instance_cell_write xr_xir_instance_slot_read xr_xir_instance_slot_write xr_xir_instance_panic_land
'''.split())
ARCHIVES = ['xray_compile_resources', 'xray_xir_admission', 'xray_xir_declarations',
            'xray_xir_scalar', 'xray_xir_runtime_host']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(command, log):
    result = subprocess.run([str(arg) for arg in command], capture_output=True, timeout=60)
    try:
        output = (result.stdout + result.stderr).decode('utf-8')
    except UnicodeDecodeError:
        output = (result.stdout + result.stderr).decode('mbcs')
    log.write_text(output, encoding='utf-8')
    return result.returncode, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'old', 'root', 'output', 'owner'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    bundle, old, root, output = (getattr(args, name).resolve() for name in ('bundle', 'old', 'root', 'output'))
    output.mkdir(parents=True, exist_ok=True)
    baseline = json.loads((old / 'baseline.json').read_text())
    assert baseline['head'] == '3c0bce6c7480126e4bcd55a5727a4224132c2b2b'
    assert len(baseline['objects']) == 139 and len(RETAINED) == 40
    assert RETAINED <= baseline['objects'].keys()
    protected = dict(baseline['files'])
    protected.update({row['object']: row['sha256'] for row in baseline['objects'].values()})
    protected[baseline['data_object']['path']] = baseline['data_object']['sha256']
    protected['baseline.json'] = sha(old / 'baseline.json')
    protected['build/old_sdk_calls.exe'] = sha(old / 'build/old_sdk_calls.exe')
    def verify_preserved():
        for name, expected in protected.items():
            assert sha(old / name) == expected, ('preserved old input changed', name)
    verify_preserved()
    code, text = execute([old / 'build/old_sdk_calls.exe'], output / 'old-sdk-positive.log')
    assert code == 0, text
    libraries = [bundle / 'lib' / (name + '.lib') for name in ARCHIVES]
    exports = set()
    for library in libraries:
        code, text = execute(['dumpbin', '/nologo', '/linkermember:1', library], output / (library.stem + '-exports.log'))
        assert code == 0, text
        exports.update(re.findall(r'\b(xr_\w+)\b', text))
    retired = set(baseline['objects']) - RETAINED
    assert len(retired) == 99 and not retired.intersection(exports), sorted(retired.intersection(exports))
    assert RETAINED <= exports, sorted(RETAINED - exports)
    assert {'xr_compile_resources_new', 'xr_compile_resources_free', 'xr_xir_compile_program_seal',
            'xr_xir_compile_program_drop', 'xr_xir_compile_default_limits'} <= exports
    outcomes = {}
    for symbol, row in baseline['objects'].items():
        executable = output / (symbol + '.exe')
        command = ['link', '/nologo', '/incremental:no', '/opt:noref', '/out:' + str(executable),
                   old / row['object'], *libraries, 'kernel32.lib', '/defaultlib:MSVCRT', '/defaultlib:OLDNAMES']
        code, text = execute(command, output / (symbol + '.log'))
        if symbol in RETAINED:
            assert code == 0, (symbol, text)
            code, text = execute([executable], output / (symbol + '-run.log'))
            assert code == 0, (symbol, text)
            outcomes[symbol] = 'retained reference links and runs'
        else:
            unresolved = re.findall(r'error LNK(?:2019|2001):([^\r\n]+)', text)
            assert code != 0 and len(unresolved) == 1 and re.search(r'\b' + re.escape(symbol) + r'\b', unresolved[0]), (symbol, unresolved, text)
            outcomes[symbol] = 'exact retired symbol unresolved; no other missing symbol'
    manifest = json.loads((bundle / 'sdk_manifest.json').read_text())
    assert [manifest[name] for name in ('wire', 'semantic', 'value_abi', 'call_abi', 'program_abi')] == [24, 63, 20, 25, 28]
    assert len(manifest['abi_measurements']) == 233
    assert [row['path'] for row in manifest['files'] if row['kind'] == 5] == sorted('lib/' + name + '.lib' for name in ARCHIVES)
    def status(name, document, expected, sdk_root=bundle):
        path = output / (name + '.json')
        path.write_text(json.dumps(document) + '\n', encoding='utf-8')
        code, text = execute([args.owner, sdk_root, path, 'status'], output / (name + '.log'))
        assert code == 0 and int(text.split()[0]) == expected, (name, code, text)
    status('current-sdk-positive', manifest, 0)
    old_manifest = json.loads((old / 'sdk/sdk_manifest.json').read_text())
    status('old-sdk-identity', old_manifest, 1, old / 'sdk')
    for name, value in [('value_abi', 17), ('call_abi', 21), ('program_abi', 26)]:
        changed = json.loads(json.dumps(manifest)); changed[name] = value
        status('old-' + name, changed, 1)
    changed = json.loads(json.dumps(manifest)); changed['abi_measurements'] = changed['abi_measurements'][:197]
    status('old-layout-recipe', changed, 1)
    # Keeping current ABI labels cannot make an old archive the same source bundle.
    changed = json.loads(json.dumps(manifest))
    old_scalar = next(row for row in old_manifest['files'] if row['path'] == 'lib/xray_xir_scalar.lib')
    for row in changed['files']:
        if row['path'] == old_scalar['path']:
            row.update(length=old_scalar['length'], sha256=old_scalar['sha256'])
    status('old-archive-current-labels', changed, 1)
    for name, source in {
        'missing-context': '#include "xir/xxir_program.h"\nvoid bad(const XrXirProgramSpec *s,XrXirProgram **p){(void)xr_xir_compile_program_seal(s,p);}\n',
        'wrong-prototype': '#include "xir/xxir_program.h"\ntypedef XrXirStatus(*Wrong)(const XrXirProgramSpec*,XrXirProgram**);\n_Static_assert(_Generic(&xr_xir_compile_program_seal,Wrong:1,default:0),"wrong prototype");\n',
    }.items():
        path = output / (name + '.c'); path.write_text(source)
        code, text = execute(['cl', '/nologo', '/std:c11', '/W4', '/WX', '/utf-8', '/MD', '/D_CRT_SECURE_NO_WARNINGS',
                              '/I' + str(bundle / 'src'), '/c', path, '/Fo' + str(output / (name + '.obj'))], output / (name + '.log'))
        assert code != 0 and ('error C2198' in text or 'error C2338' in text), (name, text)
    verify_preserved()
    result = {'preserved_head': baseline['head'], 'protected_inputs': len(protected),
              'old_positive': 'PASS', 'current_positive': 'PASS', 'retired': len(retired), 'retained': len(RETAINED),
              'objects': outcomes, 'protected_sha256': protected,
              'new_archive_sha256': {path.name: sha(path) for path in libraries},
              'old_identity_and_labels_rejected': True, 'new_layout_measurements': len(manifest['abi_measurements']),
              'scope': 'New runtime SDK exports only; absence of compiler-only APIs is not full compiler archive qualification'}
    (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"139 preserved old objects: {len(retired)} exact retired-symbol rejections, {len(RETAINED)} retained references; both SDK controls and stale identities PASS")


if __name__ == '__main__':
    main()
