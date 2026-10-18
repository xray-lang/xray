"""Execute the complete original named-file denominator through public test discovery."""
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


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--product-inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 2:
        raise ValueError('the full named consumer lane permits one or two workers')
    binary = args.binary.resolve(strict=True)
    product_inputs = FixedProductInputs(args.product_inputs, args.binary)
    args.output.mkdir(parents=True,exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='named-',dir=args.output.resolve()))
    manifest = (HERE/'runner_routes.json').read_bytes()
    rows = [r for r in json.loads(manifest)['cases'] if
            r.get('original_regression_obligation',{}).get('entry')=='test']
    spec = importlib.util.spec_from_file_location('actual_named_regression',ROOT/'scripts/run_regression_tests.py')
    runner = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = runner
    spec.loader.exec_module(runner)
    actual = {p.resolve() for p in runner.collect_cases() if runner.is_test_module(p)}
    if {ROOT/r['case_id'] for r in rows} != actual:
        raise ValueError('the complete original named input denominator changed')
    identity = subprocess.run([str(binary),'--version','--json'],capture_output=True,check=True,timeout=60)
    version = json.loads(identity.stdout)
    product_inputs.require_version(version)
    image_sha = sha(binary.read_bytes())
    environment = dict(os.environ,XRAY_STDLIB_PATH=str(product_inputs.stdlib),NO_COLOR='1')

    def run(row: dict) -> dict:
        original = ROOT/row['case_id']
        if sha(original.read_bytes().replace(b'\r\n',b'\n')) != row['source_lf_sha256']:
            raise ValueError('original source changed: '+row['case_id'])
        port = row.get('target_source_port')
        selected = ROOT/port['fixture'] if port else original
        if port and sha(selected.read_bytes().replace(b'\r\n',b'\n')) != port['lf_sha256']:
            raise ValueError('complete named target changed: '+row['case_id'])
        expected = list(row.get('named_test_migration',{}).get('mapping',{}).values()) or [
            d['name'] for d in row['original_named_test_obligation']['declarations']]
        directory = output/original.parent.name/original.stem
        directory.mkdir(parents=True,exist_ok=False)
        report_path = directory/'test-report.json'
        command = [str(binary),'test','--quiet','--jobs','1','--report',str(report_path),str(selected)]
        begun = time.monotonic()
        child = subprocess.Popen(command,cwd=directory,env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        timed_out = False
        try:
            stdout,stderr = child.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            timed_out = True
            child.kill()
            stdout,stderr = child.communicate()
        (directory/'stdout').write_bytes(stdout)
        (directory/'stderr').write_bytes(stderr)
        report = json.loads(report_path.read_text(encoding='utf-8')) if report_path.is_file() else None
        files = report.get('files',[]) if report else []
        names = [r['name'] for r in files[0].get('cases',[])] if len(files)==1 else []
        complete = (not timed_out and child.returncode==0 and len(files)==1 and names==expected
                    and all(r['status']=='passed' for r in files[0]['cases'])
                    and files[0]['tests']==len(expected) and files[0]['passed']==len(expected)
                    and not any(files[0][k] for k in ('failed','errors','timeouts','skipped')))
        result = {'case_id':row['case_id'],'source_lf_sha256':row['source_lf_sha256'],
                  'target':str(selected),'target_sha256':sha(selected.read_bytes()),'owners':row['producer_packages'],
                  'argv':command,'pid':child.pid,'exit_code':child.returncode,'timed_out':timed_out,
                  'seconds':time.monotonic()-begun,'stdout_hex':stdout.hex(),'stderr_hex':stderr.hex(),
                  'expected_names':expected,'actual_names':names,'actual_report':report,
                  'status':'PASS_COMPLETE_VM_NAMED_FILE_SCOPE' if complete else 'FAIL_POSITIVE_NAMED_CONSUMER',
                  'independent_oracle_native_and_full_resource_qualification':'OPEN_NOT_GRANTED_BY_TEST_BANNER'}
        (directory/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        return result

    records = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run,rows):
            records.append(result)
            summary = {'version':version,'product_inputs':product_inputs.evidence(),'binary_sha256':image_sha,'manifest_sha256':sha(manifest),
                       'required_files':len(rows),'completed_files':len(records),'results':records,
                       'runtime_discovery':'ACTUAL_PUBLIC_TEST_REPORT','native_qualification':'NOT_RUN_IN_THIS_LANE',
                       'production_release':False}
            (output/'result.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            if len(records)%25==0:
                print(f'{len(records)}/{len(rows)} complete named input files recorded',flush=True)
    if sha(binary.read_bytes())!=image_sha:
        raise ValueError('public executable changed during named discovery/execution')
    product_inputs.verify()
    print('Evidence: '+str(output),flush=True)
    return 0 if all(r['status']=='PASS_COMPLETE_VM_NAMED_FILE_SCOPE' for r in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
