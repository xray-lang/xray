"""Run every original rejection duty without turning migrated positives into rejections."""
from __future__ import annotations

import argparse
from fixed_product_inputs import FixedProductInputs
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT/'tests/compile_errors'))
from expected_format import expected_path_for, parse_expected, parse_diagnostics, match_assertions, ExpectedFormatError


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--product-inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 4:
        raise ValueError('this consumer lane permits one through four workers')
    binary = args.binary.resolve(strict=True)
    product_inputs = FixedProductInputs(args.product_inputs, args.binary)
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='rejection-', dir=args.output.resolve()))
    data = (HERE/'runner_routes.json').read_bytes()
    rows = [r for r in json.loads(data)['cases'] if 'original_rejection_oracle' in r]
    spec = importlib.util.spec_from_file_location('actual_rejections', ROOT/'tests/compile_errors/run_compile_error_tests.py')
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    actual = {p.resolve() for p in runner.collect_cases()}
    if {ROOT/r['case_id'] for r in rows} != actual:
        raise ValueError('the complete original rejection input denominator changed')
    identity = subprocess.run([str(binary),'--version','--json'],capture_output=True,check=True,timeout=30)
    version = json.loads(identity.stdout)
    product_inputs.require_version(version)
    image_sha = digest(binary)
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(product_inputs.stdlib), NO_COLOR='1')

    def run(row: dict) -> dict:
        source = ROOT/row['case_id']
        original = source.read_bytes()
        if hashlib.sha256(original.replace(b'\r\n',b'\n')).hexdigest() != row['source_lf_sha256']:
            raise ValueError('original input changed: '+row['case_id'])
        positive = row.get('target_disposition',{}).get('kind','').startswith('POSITIVE')
        port = row.get('target_source_port')
        selected = ROOT/port['fixture'] if port else source
        if port and hashlib.sha256(selected.read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=port['lf_sha256']:
            raise ValueError('reviewed target changed: '+row['case_id'])
        directory = output/source.parent.name/source.stem
        directory.mkdir(parents=True, exist_ok=False)
        command = [str(binary),'check',str(selected)]
        begun = time.monotonic()
        case_environment = dict(environment)
        inherited = environment.get('XRAY_TYPEPATH','')
        case_environment['XRAY_TYPEPATH'] = str(source.parent)+(os.pathsep+inherited if inherited else '')
        child = subprocess.Popen(command,cwd=directory,env=case_environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        timed_out = False
        try:
            stdout,stderr = child.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            timed_out = True
            child.kill()
            stdout,stderr = child.communicate()
        (directory/'stdout').write_bytes(stdout)
        (directory/'stderr').write_bytes(stderr)
        result = {'case_id':row['case_id'],'source_lf_sha256':row['source_lf_sha256'],
                  'target':str(selected),'target_sha256':digest(selected),'owners':row['producer_packages'],
                  'argv':command,'pid':child.pid,'exit_code':child.returncode,'timed_out':timed_out,
                  'seconds':time.monotonic()-begun,'stdout_hex':stdout.hex(),'stderr_hex':stderr.hex(),
                  'positive_target':positive,'original_oracle':row['original_rejection_oracle'],
                  'qualification':'COMPILE_DIAGNOSTIC_SCOPE_ONLY_NO_RUNTIME_FI_OR_RETIREMENT'}
        if positive:
            result['status'] = ('FAIL_POSITIVE_CONSUMER' if timed_out or child.returncode else
                                'POSITIVE_COMPILES_COMPLETE_SEMANTIC_QUALIFICATION_OPEN')
        else:
            expected, kind = expected_path_for(source)
            try:
                assertions = parse_expected(expected)
            except ExpectedFormatError as error:
                result['status'] = 'OPEN_MISSING_OR_INVALID_ORIGINAL_ORACLE'
                result['reason'] = str(error)
            else:
                diagnostics = parse_diagnostics((stdout+stderr).decode('utf-8',errors='replace'))
                failures = match_assertions(assertions,diagnostics,source.name)
                result['mismatches'] = failures
                if kind=='runtime':
                    result['status'] = 'OPEN_ORIGINAL_RUNTIME_ONLY_ORACLE'
                elif timed_out:
                    result['status'] = 'FAIL_TIMEOUT'
                elif child.returncode == 0:
                    result['status'] = 'FAIL_REQUIRED_REJECTION_ACCEPTED'
                elif failures or not diagnostics:
                    result['status'] = 'FAIL_ORIGINAL_DIAGNOSTIC_MISMATCH'
                elif any(a.misplaced or a.where.degenerate for a in assertions):
                    result['status'] = 'OPEN_ORIGINAL_DIAGNOSTIC_POSITION_GAP'
                else:
                    result['status'] = 'PASS_ORIGINAL_REJECTION_SCOPE'
        (directory/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        return result

    records = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run,rows):
            records.append(result)
            summary = {'version':version,'product_inputs':product_inputs.evidence(),'binary_sha256':image_sha,'manifest_sha256':hashlib.sha256(data).hexdigest(),
                       'required_cases':len(rows),'completed_cases':len(records),'results':records,
                       'runtime_instead_of_rejection_allowed':False,'production_release':False}
            (output/'result.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            if len(records)%50==0:
                print(f'{len(records)}/{len(rows)} actual compile consumers recorded',flush=True)
    if digest(binary)!=image_sha:
        raise ValueError('fresh public executable changed during execution')
    product_inputs.verify()
    print('Evidence: '+str(output),flush=True)
    return 0 if all(r['status']=='PASS_ORIGINAL_REJECTION_SCOPE' for r in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
