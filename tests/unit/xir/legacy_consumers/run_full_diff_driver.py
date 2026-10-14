"""Execute every original differential input while keeping unknown independent duties open."""
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

import independent_behavior_models
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
    expected_by_source = {}
    model_names = ('independent_behavior_models.py', 'independent_behavior_bindings.json',
                   'independent_scalar_models.py', 'independent_scalar_bindings.json')
    model_hashes = {name:sha((HERE/name).read_bytes()) for name in model_names}
    for r in json.loads((HERE/'oracles.json').read_text(encoding='utf-8'))['cases']:
        if 'stdout_utf8' in r['expected'] and 'exit_code' in r['expected']:
            for i in r['inputs']:
                if i['path'].startswith(('tests/diff/','tests/regression/')):
                    expected_by_source.setdefault(i['path'],r)
    # These models derive complete original run traces. Named-body adapters
    # are intentionally absent because the original run route does not call them.
    original_sources = {row['case_id'] for row in rows}
    independent_original_sources = []
    for model in independent_behavior_models.expected_rows() + independent_scalar_models.expected_rows():
        source = model['source']
        if source not in original_sources:
            continue
        previous = expected_by_source.get(source)
        if previous:
            keys = ('exit_code', 'stdout_utf8', 'stderr_utf8')
            if any(previous['expected'].get(key, '') != model['expected'][key] for key in keys):
                raise ValueError('independent original run models disagree: ' + source)
        expected_by_source[source] = model
        independent_original_sources.append(source)

    def run(row: dict) -> dict:
        source = ROOT/row['case_id']
        if sha(source.read_bytes().replace(b'\r\n',b'\n'))!=row['source_lf_sha256']:
            raise ValueError('original source changed: '+row['case_id'])
        context = row.get('original_differential_obligation',{'arguments':[],'stdin_hex':''})
        argv = runner.read_args(source) if 'original_differential_obligation' in row else []
        stdin = runner.read_stdin(source) if 'original_differential_obligation' in row else b''
        if context['arguments']!=argv or context['stdin_hex']!=stdin.hex():
            raise ValueError('original arguments/stdin changed: '+row['case_id'])
        directory = output/Path(row['case_id']).relative_to('tests')
        directory.mkdir(parents=True,exist_ok=False)
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
        if generated.is_file():result['generated_c_sha256']=sha(generated.read_bytes())
        observed = None
        if not build.returncode and not build.timed_out and native.is_file():
            result['native_sha256']=sha(native.read_bytes())
            observed = invoke('native',[native,*argv],stdin)
        oracle = expected_by_source.get(row['case_id'])
        sidecar = Path(str(source)+'.expected')
        if oracle:
            expected = oracle['expected']
            result['independent_oracle_id']=oracle['id']
            def match(r):
                return (r is not None and not r.timed_out and r.returncode==expected['exit_code']
                        and r.stdout==expected['stdout_utf8'].encode('utf-8')
                        and (r.stderr==expected['stderr_utf8'].encode('utf-8') if 'stderr_utf8' in expected else
                             expected.get('diagnostic_code','').encode('utf-8') in r.stderr
                             if expected.get('diagnostic_code') else not r.stderr))
            result['status']='PASS_INDEPENDENT_RUN_BYTE_SCOPE' if match(vm) and match(observed) and not emit.returncode else 'FAIL_INDEPENDENT_ORIGINAL_CONSUMER'
        elif sidecar.is_file():
            expected = sidecar.read_bytes()
            result['original_stdout_sidecar_sha256']=sha(expected)
            # The old sidecar does not specify independent exit/error/effect duties.
            result['status']=('OBSERVED_ORIGINAL_STDOUT_MATCH_OTHER_DUTIES_OPEN' if observed is not None
                              and not vm.timed_out and not observed.timed_out
                              and vm.stdout==expected and observed.stdout==expected else
                              'FAIL_ORIGINAL_STDOUT_SCOPE')
        else:
            result['status']='EXECUTED_INDEPENDENT_ORACLE_PENDING'
            result['parity_only']=bool(observed is not None and not vm.timed_out and not observed.timed_out
                and (vm.returncode,vm.stdout,vm.stderr)==(observed.returncode,observed.stdout,observed.stderr))
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
                     'required_inputs':len(rows),'completed_inputs':len(records),'results':records,
                     'parity_is_qualification':False,'production_release':False}
            (output/'result.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            if len(records)%25==0:print(f'{len(records)}/{len(rows)} original differential inputs recorded',flush=True)
    if sha(binary.read_bytes())!=image_sha:raise ValueError('public image changed during full original execution')
    if any(sha((HERE/name).read_bytes()) != digest for name,digest in model_hashes.items()):
        raise ValueError('independent original run model changed during execution')
    print('Evidence: '+str(output),flush=True)
    return 0 if all(r['status']=='PASS_INDEPENDENT_RUN_BYTE_SCOPE' and 'named_body_obligations' not in r for r in records) else 1


if __name__=='__main__':
    raise SystemExit(main())
