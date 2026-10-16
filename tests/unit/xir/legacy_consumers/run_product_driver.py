"""Execute preserved consumers through the public CLI and independent native images."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time

import independent_scalar_models
import independent_behavior_models
import independent_complete_models
import complete_library_model
import complete_channel_model
import complete_resource_model
import complete_typed_json_model
import complete_module_model
import complete_exception_model
import complete_aggregate_model
import complete_http_framing_model
import complete_http_policy_model
import complete_text_formats_model
import complete_scalar_events_model
import complete_async_events_model
import complete_error_boundaries_model
import complete_calendar_paths_model
import complete_collection_values_model
import complete_borrowed_views_model
import complete_collection_protocols_model
import complete_structural_inputs_model
import complete_cleanup_generator_model
import complete_callable_model
import complete_ffi_memory_model
import complete_task_regression_model
import task_rejection_model
import run_source_rejection_driver
import independent_observation_format
import extended_named_models

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0,str(ROOT/'tests/lib'))
from xraytest import proc


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--case', action='append', default=[])
    parser.add_argument('--source-rejection-owner',type=Path)
    parser.add_argument('--source-rejection-manifest',type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    parent = args.output.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=parent))
    manifest_bytes = (HERE/'product_driver_cases.json').read_bytes()
    manifest = json.loads(manifest_bytes)
    oracle_bytes = (HERE/'oracles.json').read_bytes()
    model_paths = [HERE/name for name in ('independent_scalar_models.py', 'independent_scalar_bindings.json',
                                         'independent_behavior_models.py', 'independent_behavior_bindings.json',
                                         'independent_observation_format.py', 'extended_named_models.py',
                                         'extended_named_bindings.json', 'structured_named_models.json',
                                         'independent_complete_models.py', 'complete_program_bindings.json',
                                         'complete_numeric_model.py', 'complete_text_range_model.py',
                                         'complete_ownership_model.py', 'complete_library_model.py', 'complete_channel_model.py', 'complete_resource_model.py', 'complete_typed_json_model.py', 'complete_module_model.py', 'complete_exception_model.py', 'complete_aggregate_model.py', 'complete_http_framing_model.py', 'complete_http_policy_model.py', 'complete_text_formats_model.py', 'complete_scalar_events_model.py', 'complete_async_events_model.py', 'complete_error_boundaries_model.py', 'complete_calendar_paths_model.py', 'complete_collection_values_model.py', 'complete_borrowed_views_model.py', 'complete_collection_protocols_model.py', 'complete_structural_inputs_model.py', 'structural_inputs_bindings.json', 'complete_cleanup_generator_model.py', 'complete_callable_model.py', 'complete_ffi_memory_model.py', 'complete_task_regression_model.py', 'task_rejection_model.py', 'run_source_rejection_driver.py')]
    model_digests = {path.name:sha(path.read_bytes()) for path in model_paths}
    oracles = {row['id']:row for row in json.loads(oracle_bytes)['cases']}
    for row in (independent_scalar_models.expected_rows() + independent_behavior_models.expected_rows()
                + independent_behavior_models.named_expected_rows() + extended_named_models.expected_rows()
                + independent_complete_models.expected_rows() + complete_library_model.expected_rows() + complete_channel_model.expected_rows() + complete_resource_model.expected_rows() + complete_callable_model.expected_rows() + complete_ffi_memory_model.expected_rows() + complete_task_regression_model.expected_rows() + complete_task_regression_model.named_expected_rows() + complete_cleanup_generator_model.expected_rows() + complete_typed_json_model.expected_rows() + complete_module_model.expected_rows() + complete_exception_model.expected_rows() + complete_aggregate_model.expected_rows() + complete_http_framing_model.expected_rows() + complete_http_policy_model.expected_rows() + complete_text_formats_model.expected_rows() + complete_scalar_events_model.expected_rows() + complete_async_events_model.expected_rows() + complete_error_boundaries_model.expected_rows() + complete_calendar_paths_model.expected_rows() + complete_collection_values_model.expected_rows() + complete_borrowed_views_model.expected_rows() + complete_collection_protocols_model.expected_rows() + complete_structural_inputs_model.expected_rows()):
        if row['id'] in oracles:
            raise ValueError('independent scalar oracle identity collides with a preserved oracle')
        oracles[row['id']] = row
    if task_rejection_model.IDENTITY in oracles:raise ValueError('Controlled rejection identity collides with a preserved oracle')
    oracles[task_rejection_model.IDENTITY] = task_rejection_model.expected()
    cases = manifest['cases']
    if any(row['entry'] not in ('run', 'function', 'test', 'source_rejection') for row in cases):
        raise ValueError('consumer entry must name an existing run/function/test route')
    if args.case:
        if len(args.case) != len(set(args.case)) or set(args.case)-{row['id'] for row in cases}:
            raise ValueError('case selection must name distinct preserved manifest entries')
        cases = [row for row in cases if row['id'] in args.case]
    records = []
    results = []
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(ROOT/'stdlib'), NO_COLOR='1', PYTHONUTF8='1')
    binary_digest = sha(binary.read_bytes())
    version = None

    def save() -> None:
        (output/'result.json').write_text(json.dumps({
            'scope':'ACTUAL_PUBLIC_CLI_CONSUMERS_NOT_FULL_PRODUCT_QUALIFICATION',
            'binary':str(binary), 'binary_sha256':binary_digest, 'version':version,
            'manifest_sha256':sha(manifest_bytes), 'oracles_sha256':sha(oracle_bytes), 'selected_cases':len(cases),
            'independent_scalar_model_sha256':model_digests['independent_scalar_models.py'],
            'independent_scalar_bindings_sha256':model_digests['independent_scalar_bindings.json'],
            'independent_behavior_model_sha256':model_digests['independent_behavior_models.py'],
            'independent_behavior_bindings_sha256':model_digests['independent_behavior_bindings.json'],
            'independent_observation_format_sha256':model_digests['independent_observation_format.py'],
            'extended_named_model_sha256':model_digests['extended_named_models.py'],
            'extended_named_bindings_sha256':model_digests['extended_named_bindings.json'],
            'structured_named_models_sha256':model_digests['structured_named_models.json'],
            'complete_program_model_sha256':model_digests['independent_complete_models.py'],
            'complete_program_bindings_sha256':model_digests['complete_program_bindings.json'],
            'complete_numeric_model_sha256':model_digests['complete_numeric_model.py'],
            'complete_text_range_model_sha256':model_digests['complete_text_range_model.py'],
            'complete_ownership_model_sha256':model_digests['complete_ownership_model.py'],
            'complete_library_model_sha256':model_digests['complete_library_model.py'],
            'complete_channel_model_sha256':model_digests['complete_channel_model.py'],
            'complete_resource_model_sha256':model_digests['complete_resource_model.py'],
            'complete_typed_json_model_sha256':model_digests['complete_typed_json_model.py'],
            'complete_module_model_sha256':model_digests['complete_module_model.py'],
            'complete_exception_model_sha256':model_digests['complete_exception_model.py'],
            'complete_aggregate_model_sha256':model_digests['complete_aggregate_model.py'],
            'complete_http_framing_model_sha256':model_digests['complete_http_framing_model.py'],
            'complete_http_policy_model_sha256':model_digests['complete_http_policy_model.py'],
            'complete_text_formats_model_sha256':model_digests['complete_text_formats_model.py'],
            'complete_scalar_events_model_sha256':model_digests['complete_scalar_events_model.py'],
            'complete_async_events_model_sha256':model_digests['complete_async_events_model.py'],
            'complete_error_boundaries_model_sha256':model_digests['complete_error_boundaries_model.py'],
            'complete_calendar_paths_model_sha256':model_digests['complete_calendar_paths_model.py'],
            'complete_collection_values_model_sha256':model_digests['complete_collection_values_model.py'],
            'complete_borrowed_views_model_sha256':model_digests['complete_borrowed_views_model.py'],
            'complete_collection_protocols_model_sha256':model_digests['complete_collection_protocols_model.py'],
            'complete_structural_inputs_model_sha256':model_digests['complete_structural_inputs_model.py'],
            'structural_inputs_bindings_sha256':model_digests['structural_inputs_bindings.json'],
            'complete_cleanup_generator_model_sha256':model_digests['complete_cleanup_generator_model.py'],
            'complete_callable_model_sha256':model_digests['complete_callable_model.py'],
            'complete_ffi_memory_model_sha256':model_digests['complete_ffi_memory_model.py'],
            'complete_task_regression_model_sha256':model_digests['complete_task_regression_model.py'],
            'evidence_directory':str(output),
            'cases':results, 'commands':records,
            'full_fi_lifetime_physical_sanitizer':'NOT_RUN', 'production_release':False,
        }, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')

    def invoke(label: str, argv: list[str], stdin: bytes = b'', cwd: Path = ROOT) -> dict:
        started = time.monotonic()
        observed = proc.run(argv,cwd=cwd,env=environment,stdin=stdin,timeout=60)
        stdout,stderr,timed_out = observed.stdout,observed.stderr,observed.timed_out
        (output/(label+'.stdout')).write_bytes(stdout)
        (output/(label+'.stderr')).write_bytes(stderr)
        row = {'label':label, 'argv':argv, 'cwd':str(cwd), 'exit_code':observed.returncode,
               'timed_out':timed_out, 'seconds':time.monotonic()-started,
               'stdout_hex':stdout.hex(), 'stderr_hex':stderr.hex()}
        records.append(row)
        save()
        return row

    def matches(row: dict, expected: dict) -> bool:
        if row['timed_out'] or any(row[key] != expected[key] for key in ('exit_code','stdout_hex')):
            return False
        if 'stderr_required_utf8' in expected:
            stderr = bytes.fromhex(row['stderr_hex'])
            return all(value.encode('utf-8') in stderr for value in expected['stderr_required_utf8'])
        return row['stderr_hex'] == expected['stderr_hex']

    try:
        v = invoke('version',[str(binary),'--version','--json'])
        if v['exit_code'] or v['stderr_hex'] or v['timed_out']:
            raise ValueError('actual public CLI identity query failed')
        version = json.loads(bytes.fromhex(v['stdout_hex']))
        if version.get('product') != 'xray-lang' or not {'vm','aot'} <= set(version.get('features',[])):
            raise ValueError('binary does not declare the required real product routes')
        for case in cases:
            if case['entry'] == 'source_rejection':
                bound = oracles[task_rejection_model.IDENTITY]
                if (case['id'] != bound['id'] or case['oracle_id'] != bound['id'] or case['source'] != bound['source']
                        or case['source_lf_sha256'] != bound['source_lf_sha256']):
                    raise ValueError('Formal rejection entry lost its exact controlled source')
                row = run_source_rejection_driver.run_group(binary,args.compiler,args.source_rejection_owner,
                    args.source_rejection_manifest,output/('rejection-'+sha(case['id'].encode())[:16]))
                results.append(row);save();print(case['id']+': '+row['status'],flush=True);continue
            oracle = oracles[case['oracle_id']]
            independent_complete_models.require_target(oracle, version.get('target'))
            evidence = output/('case-'+sha(case['id'].encode('utf-8'))[:16])
            evidence.mkdir()
            original = ROOT/case['source']
            source_bytes = original.read_bytes()
            if sha(source_bytes.replace(b'\r\n',b'\n')) != case['source_lf_sha256']:
                raise ValueError('preserved source changed: '+case['source'])
            preserved_run = case.get('preserved_original_run')
            if preserved_run:
                original_bytes = (ROOT/preserved_run['source']).read_bytes().replace(b'\r\n',b'\n')
                if (sha(original_bytes) != preserved_run['source_lf_sha256']
                        or source_bytes.replace(b'\r\n',b'\n') != original_bytes + preserved_run['observation_suffix'].encode('utf-8')):
                    raise ValueError('the full original run body or observation suffix changed')
            closure = evidence/'source'
            target = closure/case['source']
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source_bytes)
            for companion in case['companions']:
                data = (ROOT/companion['path']).read_bytes()
                if sha(data.replace(b'\r\n',b'\n')) != companion['lf_sha256']:
                    raise ValueError('preserved companion changed: '+companion['path'])
                path = closure/companion['path']
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            source = source_bytes.decode('utf-8')
            original_context = case.get('source_context') == 'original_absolute'
            if original_context:
                if case['entry'] != 'run':
                    raise ValueError('original context cannot overwrite or adapt a preserved source')
                target = original
            suffix = ''
            if case['entry'] == 'function':
                value = oracle['expected']['return_i64']
                entry = case.get('entry_function', 'answer')
                if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', entry):
                    raise ValueError('function entry must be one source declaration identifier')
                suffix = f'\nvar observedResult = {entry}()\nassertEqual(observedResult, {value})\nprint(observedResult)\n'
            if not original_context:
                target.write_text(source+suffix, encoding='utf-8', newline='\n')
            expected = {'exit_code':case.get('expected_exit_code',0),
                        'stdout_hex':case['stdout_utf8'].encode('utf-8').hex(),
                        'stderr_hex':case.get('stderr_utf8','').encode('utf-8').hex()}
            if case.get('stderr_required_utf8'):
                expected.pop('stderr_hex')
                expected['stderr_required_utf8'] = case['stderr_required_utf8']
            if oracle.get('model'):
                computed = oracle['expected']
                if (expected['stdout_hex'] != computed['stdout_utf8'].encode('utf-8').hex()
                        or expected.get('stderr_hex') != computed['stderr_utf8'].encode('utf-8').hex()
                        or expected['exit_code'] != computed['exit_code']):
                    raise ValueError('manifest differs from the independent arithmetic model')
            row = {'id':case['id'],'oracle_id':case['oracle_id'],'source':case['source'],
                   'source_lf_sha256':case['source_lf_sha256'],
                   'entry_sha256':sha(target.read_bytes()),'entry_adapter_suffix':suffix,
                   'owners':case['owners'],'expected':expected,'routes':{},
                   'remaining_gates':case['remaining_gates']}
            if oracle.get('model'):
                row['independent_arithmetic_model'] = oracle['model']
            if preserved_run:
                row['preserved_original_run'] = preserved_run
            if case.get('independent_observations'):
                observations = case['independent_observations']
                observed_source = ROOT/observations['selected_complete_source']
                if sha(observed_source.read_bytes().replace(b'\r\n',b'\n')) != observations['selected_source_lf_sha256']:
                    raise ValueError('original complete observation source changed')
                names = oracle['expected']['required_test_names']
                values = oracle['expected']['per_test_observations']
                for name in names:
                    actual = [z['expected'] for z in observations['values'] if z['test']==name]
                    if actual != values[name]['values']:
                        raise ValueError('observed values differ from the frozen independent model')
                initialization = observations.get('module_initialization_stdout_utf8','')
                if initialization != oracle['expected'].get('module_initialization_stdout_utf8',''):
                    raise ValueError('original module initialization observations differ from their frozen model')
                expected_bytes = (initialization + ''.join(independent_observation_format.render(z)+'\n'
                                         for z in observations['values'])).encode('utf-8')
                if expected_bytes.hex() != expected['stdout_hex']:
                    raise ValueError('independent observation bytes changed')
                row['independent_observations'] = observations
            results.append(row)
            arguments = case.get('arguments',[])
            stdin = bytes.fromhex(case.get('stdin_hex',''))
            workspace = evidence/'workspace'
            workspace.mkdir()
            if case['entry'] == 'test':
                # Native projection invokes the same complete bodies in order;
                # real CLI test discovery is checked independently before that.
                # The report writer uses the Windows narrow FILE interface.
                # Avoid repeating a long consumer identity in its absolute path.
                report = output/(sha(case['id'].encode('utf-8'))[:16]+'-test-report.json')
                run = invoke(case['id']+'-test', [str(binary),'test','--quiet','--jobs','1',
                                                '--report',str(report),str(target)])
                names = oracle['expected']['required_test_names']
                discovered = json.loads(report.read_text(encoding='utf-8')) if report.is_file() else None
                row['test_report'] = discovered
                files = discovered.get('files',[]) if discovered else []
                qualified = (run['exit_code']==0 and not run['timed_out'] and not run['stderr_hex']
                             and len(files)==1 and [z['name'] for z in files[0]['cases']]==names
                             and all(z['status']=='passed' for z in files[0]['cases'])
                             and files[0]['tests']==len(names) and files[0]['passed']==len(names)
                             and not any(files[0][k] for k in ('failed','errors','timeouts','skipped')))
                row['routes']['cli_test'] = 'PASS' if qualified else 'FAIL_POSITIVE_CONSUMER'
                suffix = '\n'+'\n'.join(name+'()' for name in names)+'\n'
                target.write_text(source+suffix, encoding='utf-8', newline='\n')
                row['entry_adapter_suffix'] = suffix
                row['entry_sha256'] = sha(target.read_bytes())
            vm_argv = [str(binary),'run',str(target)] + (['--',*arguments] if arguments else [])
            vm = invoke(case['id']+'-vm',vm_argv,stdin,workspace)
            row['routes']['vm'] = 'PASS' if matches(vm,expected) else 'FAIL_POSITIVE_CONSUMER'
            generated = evidence/'program.c'
            emit = invoke(case['id']+'-emit',[str(binary),'build','--c-only',str(target),'-o',str(generated)],cwd=workspace)
            c = generated.read_bytes() if generated.is_file() else b''
            emit_message = ('Generated: '+str(generated)+'\n').encode('utf-8').hex()
            c_ok = (emit['exit_code']==0 and not emit['timed_out'] and emit['stdout_hex']==emit_message
                    and not emit['stderr_hex'] and b'const XrXirProgramSpec xray_app_program' in c
                    and b'XrXirCallEntry' in c and b'XrProto' not in c and b'xrt_vm_run' not in c
                    and not re.search(rb'\(\s*\{',c))
            row['routes']['c_projection'] = 'PASS' if c_ok else 'FAIL_POSITIVE_CONSUMER'
            row['generated_c_sha256'] = sha(c) if c else None
            native = evidence/'program.exe'
            command = [str(binary),'build',str(target),'-o',str(native)]
            if args.compiler:
                command += ['--cc',str(args.compiler.resolve(strict=True))]
            built = invoke(case['id']+'-build',command,cwd=workspace)
            build_message = ('Built: '+str(native)+'\n').encode('utf-8').hex()
            if (built['exit_code']==0 and not built['timed_out'] and not built['stderr_hex']
                    and built['stdout_hex']==build_message and native.is_file()):
                native_digest = sha(native.read_bytes())
                result = invoke(case['id']+'-native',[str(native),*arguments],stdin,workspace)
                row['routes']['native'] = 'PASS' if (matches(result,expected)
                    and sha(native.read_bytes())==native_digest) else 'FAIL_POSITIVE_CONSUMER'
                row['native_sha256'] = native_digest
            else:
                row['routes']['native'] = 'FAIL_BUILD_POSITIVE_CONSUMER'
            row['status'] = 'PASS_SCOPED_CONSUMER' if all(v=='PASS' for v in row['routes'].values()) else 'FAIL_POSITIVE_CONSUMER'
            save()
            print(case['id']+': '+json.dumps(row['routes']), flush=True)
        if sha(binary.read_bytes()) != binary_digest:
            raise ValueError('public executable changed while consumers ran')
        if any(sha(path.read_bytes()) != model_digests[path.name] for path in model_paths):
            raise ValueError('independent model inputs changed while consumers ran')
        print('Evidence: '+str(output), flush=True)
        return 0 if all(row['status'] in ('PASS_SCOPED_CONSUMER','PASS_INDEPENDENT_CHECKED_REJECTION_SCOPE') for row in results) else 1
    finally:
        save()


if __name__ == '__main__':
    raise SystemExit(main())
