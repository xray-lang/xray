"""Execute every original input identity through its reviewed source and exact oracle."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time

import audit
import complete_target_selection
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
import independent_scalar_models

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0,str(ROOT/'tests/lib'))
from xraytest import proc


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=2)
    parser.add_argument('--source-rejection-owner',type=Path)
    parser.add_argument('--source-rejection-manifest',type=Path)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 2:
        raise ValueError('full original differential lane permits one or two workers')
    binary = args.binary.resolve(strict=True)
    compiler = args.compiler.resolve(strict=True)
    args.output.mkdir(parents=True,exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='diff-',dir=args.output.resolve()))
    manifest_bytes = (HERE/'runner_routes.json').read_bytes()
    rows = [r for r in json.loads(manifest_bytes)['cases'] if
            any(e['suite']=='differential' for e in r['original_runner_entries']) or
            r.get('original_regression_obligation',{}).get('entry')=='run']
    spec = importlib.util.spec_from_file_location('actual_full_diff',ROOT/'tests/diff/run_backend_diff.py')
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    actual = {p.resolve() for p in runner.collect_cases('',str(runner.SCRIPT_DIR/'coro_regression_cases.txt'))}
    reg_spec = importlib.util.spec_from_file_location('actual_standalone_regression',ROOT/'scripts/run_regression_tests.py')
    regression = importlib.util.module_from_spec(reg_spec)
    sys.modules[reg_spec.name] = regression
    reg_spec.loader.exec_module(regression)
    actual |= {p.resolve() for p in regression.collect_cases() if not regression.is_test_module(p)}
    if {ROOT/r['case_id'] for r in rows} != actual:
        raise ValueError('full original differential denominator changed')
    identity = proc.run([binary,'--version','--json'],timeout=30)
    if identity.returncode or identity.stderr or identity.timed_out:
        raise ValueError('fresh public CLI identity query failed')
    version = json.loads(identity.stdout)
    image_sha = sha(binary.read_bytes())
    environment = dict(os.environ,XRAY_STDLIB_PATH=str(ROOT/'stdlib'),NO_COLOR='1')
    preserved = json.loads((HERE/'oracles.json').read_text(encoding='utf-8'))['cases']
    audit.validate_oracles({'cases': preserved})
    route_data = json.loads(manifest_bytes)
    checked = audit.validate_runner_routes(route_data, audit.read_json(HERE/'responsibilities.json'))
    audit.require_full_explicit_runner_coverage(checked)
    model_names = ('independent_behavior_models.py', 'independent_behavior_bindings.json',
                   'independent_scalar_models.py', 'independent_scalar_bindings.json',
                   'independent_complete_models.py', 'complete_program_bindings.json',
                   'complete_numeric_model.py', 'complete_text_range_model.py',
                   'complete_ownership_model.py', 'complete_library_model.py', 'complete_channel_model.py', 'complete_resource_model.py', 'complete_typed_json_model.py', 'complete_module_model.py', 'complete_exception_model.py', 'complete_aggregate_model.py', 'complete_http_framing_model.py', 'complete_http_policy_model.py', 'complete_text_formats_model.py', 'complete_scalar_events_model.py', 'complete_async_events_model.py', 'complete_error_boundaries_model.py', 'complete_calendar_paths_model.py', 'complete_collection_values_model.py', 'complete_borrowed_views_model.py', 'complete_collection_protocols_model.py', 'complete_structural_inputs_model.py', 'structural_inputs_bindings.json', 'complete_cleanup_generator_model.py', 'complete_callable_model.py', 'complete_ffi_memory_model.py', 'complete_task_regression_model.py', 'task_rejection_model.py', 'run_source_rejection_driver.py', 'complete_target_selection.py',
                   'audit.py', 'oracles.json', 'run_full_diff_driver.py')
    model_hashes = {name:sha((HERE/name).read_bytes()) for name in model_names}
    selections = {row['case_id']: complete_target_selection.select(row, ROOT, runner) for row in rows}
    if len(selections) != len(rows):
        raise ValueError('original consumer identity was duplicated during target selection')
    models = (independent_behavior_models.expected_rows() + independent_scalar_models.expected_rows()
              + independent_complete_models.expected_rows() + complete_library_model.expected_rows() + complete_channel_model.expected_rows() + complete_resource_model.expected_rows() + complete_callable_model.expected_rows() + complete_ffi_memory_model.expected_rows() + complete_task_regression_model.expected_rows() + complete_cleanup_generator_model.expected_rows() + complete_typed_json_model.expected_rows() + complete_module_model.expected_rows() + complete_exception_model.expected_rows() + complete_aggregate_model.expected_rows() + complete_http_framing_model.expected_rows() + complete_http_policy_model.expected_rows() + complete_text_formats_model.expected_rows() + complete_scalar_events_model.expected_rows() + complete_async_events_model.expected_rows() + complete_error_boundaries_model.expected_rows() + complete_calendar_paths_model.expected_rows() + complete_collection_values_model.expected_rows() + complete_borrowed_views_model.expected_rows() + complete_collection_protocols_model.expected_rows() + complete_structural_inputs_model.expected_rows())
    for model in models:
        independent_complete_models.require_target(model, version.get('target'))
    expected_by_source = {identity:complete_target_selection.expected_for(selected, preserved, models)
                          for identity,selected in selections.items()}
    rejection_by_source = {identity:task_rejection_model.for_selection(selection)
                           for identity,selection in selections.items() if selection['rejection_oracle_required']}
    independent_original_sources = [identity for identity,selected in selections.items()
                                    if expected_by_source[identity] is not None
                                    and not selected['canonical_port_selected']]
    independent_canonical_sources = [identity for identity,selected in selections.items()
                                     if expected_by_source[identity] is not None
                                     and selected['canonical_port_selected']]

    def run(row: dict) -> dict:
        selected = selections[row['case_id']]
        if complete_target_selection.select(row, ROOT, runner) != selected:
            raise ValueError('reviewed consumer selection changed before execution')
        original_source = ROOT/row['case_id']
        source = ROOT/selected['selected_source']
        argv = selected['arguments']
        stdin = bytes.fromhex(selected['stdin_hex'])
        directory = output/Path(row['case_id']).relative_to('tests')
        directory.mkdir(parents=True,exist_ok=False)
        if rejection_by_source.get(row['case_id']):
            result = run_source_rejection_driver.run_group(binary,compiler,args.source_rejection_owner,
                args.source_rejection_manifest,directory/'checked-rejection',timeout=30)
            result.update(case_id=row['case_id'],selection=selected,owners=row['producer_packages'],
                          original_entries=row['original_runner_entries'],arguments=argv,stdin_hex=stdin.hex())
            if complete_target_selection.select(row,ROOT,runner) != selected:
                raise ValueError('Controlled rejection source or input context changed')
            (directory/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            return result
        commands = []

        def invoke(label: str,command: list,raw_input: bytes = b''):
            begun = time.monotonic()
            result = proc.run(command,cwd=directory,env=environment,stdin=raw_input,timeout=30)
            (directory/(label+'.stdout')).write_bytes(result.stdout)
            (directory/(label+'.stderr')).write_bytes(result.stderr)
            commands.append({'label':label,'argv':list(map(str,command)),'cwd':str(directory),
                             'exit_code':result.returncode,'timed_out':result.timed_out,
                             'seconds':time.monotonic()-begun,'stdout_hex':result.stdout.hex(),
                             'stderr_hex':result.stderr.hex()})
            return result

        vm = invoke('vm',[binary,'run',source,*(['--',*argv] if argv else [])],stdin)
        generated = directory/'program.c'
        emit = invoke('emit',[binary,'build','--c-only',source,'-o',generated])
        native = directory/'program.exe'
        build = invoke('build',[binary,'build',source,'-o',native,'--cc',compiler])
        result = {'case_id':row['case_id'],'source_lf_sha256':row['source_lf_sha256'],
                  'owners':row['producer_packages'],'arguments':argv,'stdin_hex':stdin.hex(),
                  'commands':commands,'original_entries':row['original_runner_entries'],
                  'effect_lifetime_resource_provider_fi_qualification':'OPEN_NOT_PROVED_BY_PARITY'}
        result['selection'] = selected
        if generated.is_file():result['generated_c_sha256']=sha(generated.read_bytes())
        observed = None
        if not build.returncode and not build.timed_out and native.is_file():
            result['native_sha256']=sha(native.read_bytes())
            observed = invoke('native',[native,*argv],stdin)
        oracle = expected_by_source.get(row['case_id'])
        sidecar = Path(str(original_source)+'.expected')
        original_stdout = sidecar.read_bytes() if sidecar.is_file() else None
        if original_stdout is not None:
            result['original_stdout_sidecar_sha256'] = sha(original_stdout)
        projection = complete_target_selection.projection_valid(emit, generated)
        result['actual_current_C_projection_valid'] = projection
        result['status'] = complete_target_selection.observation_status(
            selected, oracle, vm, observed, projection, original_stdout)
        if oracle:
            result['independent_oracle_id'] = oracle['id']
        else:
            result['parity_only'] = bool(observed is not None and not vm.timed_out and not observed.timed_out
                and (vm.returncode,vm.stdout,vm.stderr)==(observed.returncode,observed.stdout,observed.stderr))
        if complete_target_selection.select(row, ROOT, runner) != selected:
            raise ValueError('reviewed consumer source or input context changed during execution')
        if row.get('original_regression_obligation',{}).get('entry')=='test':
            result['named_body_obligations']='OPEN: original run route does not discover or execute named bodies'
        (directory/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        return result

    records = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run,rows):
            records.append(result)
            summary={'version':version,'binary_sha256':image_sha,'manifest_sha256':sha(manifest_bytes),
                     'independent_model_sha256':model_hashes,
                     'independent_original_sources':independent_original_sources,
                     'independent_canonical_sources':independent_canonical_sources,
                     'independent_checked_rejection_sources':[identity for identity,value in rejection_by_source.items() if value],
                     'normal_output_and_checked_rejection_scopes_separate':True,
                     'canonical_selected_inputs':sum(s['canonical_port_selected'] for s in selections.values()),
                     'scope':'ORIGINAL_INPUT_IDENTITIES_WITH_REVIEWED_EXACT_TARGET_SELECTION',
                     'required_inputs':len(rows),'completed_inputs':len(records),'results':records,
                     'parity_is_qualification':False,'production_release':False}
            (output/'result.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            if len(records)%25==0:print(f'{len(records)}/{len(rows)} original input identities recorded',flush=True)
    if sha(binary.read_bytes())!=image_sha:raise ValueError('public image changed during full original execution')
    if any(sha((HERE/name).read_bytes()) != digest for name,digest in model_hashes.items()):
        raise ValueError('independent original run model changed during execution')
    if sha((HERE/'runner_routes.json').read_bytes()) != sha(manifest_bytes):
        raise ValueError('consumer routing changed during full execution')
    print('Evidence: '+str(output),flush=True)
    return 0 if all(r['status'] in ('PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE','PASS_INDEPENDENT_CHECKED_REJECTION_SCOPE') and 'named_body_obligations' not in r for r in records) else 1


if __name__=='__main__':
    raise SystemExit(main())
