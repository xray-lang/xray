"""Run unchanged parallel-api originals against one closed public product input."""
from __future__ import annotations
import argparse,hashlib,json,os,re,sys,tempfile,time
from pathlib import Path
from types import SimpleNamespace

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
sys.path.insert(0,str(ROOT/'tests/lib'))
from xraytest import proc
from fixed_product_inputs import FixedProductInputs
import complete_parallel_api_model as model


def validate_cases(bound,oracles=None):
    if oracles is None:
        oracles={row["id"]:row for row in model.expected_rows()}
    cases=bound["cases"]
    if {row["id"] for row in cases}!=set(oracles) or len(cases)!=len(oracles):
        raise ValueError("Parallel API manifest lost a complete original")
    for row in cases:
        oracle=oracles[row['id']]
        if (row['oracle_id']!=oracle['id'] or row['source']!=oracle['source'] or
                row['source_lf_sha256']!=model._INPUTS[row['source']] or
                row['entry']!='run' or row['source_context']!='original_absolute' or
                row['arguments']!=[] or row['stdin_hex']!='' or row['companions']!=[] or
                row['stdout_utf8']!=oracle['expected']['stdout_utf8'] or
                row['stdout_utf8_options']!=oracle['expected']['stdout_utf8_options']):
            raise ValueError('Complete parallel-api manifest changed an original input')


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--product-inputs',type=Path,required=True)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--case',action='append',default=[])
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    fixed=FixedProductInputs(args.product_inputs,args.binary)
    binary=args.binary.resolve(strict=True)
    compiler=args.compiler.resolve(strict=True)
    bound=json.loads((HERE/'parallel_api_driver_cases.json').read_text(encoding='utf-8'))
    oracles={row['id']:row for row in model.expected_rows()}
    cases=bound['cases']
    if {row['id'] for row in cases}!=set(oracles) or len(cases)!=len(oracles):
        raise ValueError('Complete parallel-api consumer manifest lost a program')
    validate_cases(bound,oracles)
    if args.case:
        if set(args.case)-set(oracles):
            raise ValueError('Unknown complete parallel-api case')
        cases=[row for row in cases if row['id'] in args.case]
    args.output.mkdir(parents=True,exist_ok=True)
    output=Path(tempfile.mkdtemp(prefix='parallel-api-',dir=args.output.resolve()))
    protected=[Path(__file__),HERE/'parallel_api_driver_cases.json',HERE/'complete_parallel_api_model.py',
               HERE/'fixed_product_inputs.py',*[ROOT/p for p in model._INPUTS]]
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    before={str(p):digest(p) for p in protected}
    records=[]
    results=[]
    env=dict(os.environ,XRAY_STDLIB_PATH=str(fixed.stdlib),NO_COLOR='1',PYTHONUTF8='1')
    def save():
        (output/'result.json').write_text(json.dumps({
            'scope':'COMPLETE_REAL_PARALLEL_API_ORIGINALS_INDEPENDENT_OUTPUTS_NOT_FULL_QUALIFICATION',
            'consumer_code_files':before,'fixed_product':fixed.evidence(),
            'cases':results,'commands':records,'full_qualification':False,
            'runtime_FI_lifetime_physical_sanitizer_provider_mixed':'NOT_RUN',
            'production_release':False},ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    def invoke(label,argv):
        started=time.monotonic()
        actual=proc.run(argv,cwd=ROOT,env=env,stdin=b'',timeout=60)
        row={'label':label,'argv':[str(x) for x in argv],'cwd':str(ROOT),
             'exit_code':actual.returncode,'timed_out':actual.timed_out,
             'seconds':time.monotonic()-started,
             'stdout_hex':actual.stdout.hex(),'stderr_hex':actual.stderr.hex()}
        records.append(row)
        (output/(label+'.stdout')).write_bytes(actual.stdout)
        (output/(label+'.stderr')).write_bytes(actual.stderr)
        save()
        return actual
    try:
        identity=invoke('version',[binary,'--version','--json'])
        if not identity.ok or identity.stderr:
            raise ValueError('Public parallel-api product identity query failed')
        fixed.require_version(json.loads(identity.stdout))
        for case in cases:
            oracle=oracles[case['id']]
            model.expected_outputs(oracle)
            source=ROOT/case['source']
            raw=source.read_bytes()
            if hashlib.sha256(raw.replace(b'\r\n',b'\n')).hexdigest()!=case['source_lf_sha256']:
                raise ValueError('Whole original parallel-api body changed')
            target=output/case['id']
            target.mkdir()
            c=target/'program.c'
            exe=target/'program.exe'
            row={'id':case['id'],'source':case['source'],
                 'source_raw_sha256':digest(source),'source_lf_sha256':case['source_lf_sha256'],
                 'source_context':'original_absolute','arguments':[],'stdin_hex':'',
                 'expected':oracle['expected'],'observation_count':oracle['observation_count'],
                 'owners':case['owners'],'routes':{}}
            results.append(row)
            vm=invoke(case['id']+'-vm',[binary,'run',source])
            row['routes']['vm']='PASS' if model.matches(oracle,vm) else 'FAIL_POSITIVE_CONSUMER'
            emitted=invoke(case['id']+'-emit',[binary,'build','--c-only',source,'-o',c])
            generated=c.read_bytes() if c.is_file() else b''
            c_ok=(emitted.ok and not emitted.stderr and emitted.stdout==('Generated: '+str(c)+'\n').encode() and
                  b'const XrXirProgramSpec xray_app_program' in generated and b'XrXirCallEntry' in generated and
                  b'XrProto' not in generated and b'xrt_vm_run' not in generated and not re.search(rb'\(\s*\{',generated))
            row['routes']['c_projection']='PASS' if c_ok else 'FAIL_POSITIVE_CONSUMER'
            row['generated_c_sha256']=digest(c) if generated else None
            built=invoke(case['id']+'-build',[binary,'build',source,'-o',exe,'--cc',compiler])
            if built.ok and not built.stderr and built.stdout==('Built: '+str(exe)+'\n').encode() and exe.is_file():
                before_native=digest(exe)
                native=invoke(case['id']+'-native',[exe])
                row['native_sha256']=before_native
                row['routes']['native']='PASS' if model.matches(oracle,native) and digest(exe)==before_native else 'FAIL_POSITIVE_CONSUMER'
            else:
                row['routes']['native']='FAIL_BUILD_POSITIVE_CONSUMER'
            row['status']='PASS_SCOPED_CONSUMER' if all(x=='PASS' for x in row['routes'].values()) else 'FAIL_POSITIVE_CONSUMER'
            if source.read_bytes()!=raw:
                raise ValueError('Original parallel-api source changed during execution')
            fixed.verify()
            save()
            print(case['id']+': '+row['status'],flush=True)
        return 0 if all(x['status']=='PASS_SCOPED_CONSUMER' for x in results) else 1
    finally:
        fixed.verify()
        if before!={str(p):digest(p) for p in protected}:
            raise ValueError('Protected parallel-api consumer or product inputs changed')
        save()


if __name__=='__main__':
    raise SystemExit(main())
