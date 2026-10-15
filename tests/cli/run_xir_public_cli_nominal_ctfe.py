"""Exercise every required nominal constant and report unresolved cases as failures."""
from __future__ import annotations
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path
from xir_no_child_process import NoChildProcess
from xir_nominal_ctfe_cases import CASES


def main(cases=CASES, negative_cases=None, family='NOMINAL_CTFE', case_semantics=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path);args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve();binary.relative_to(root);temporary=None
    if args.output:output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:temporary=tempfile.TemporaryDirectory(prefix='nominal-ctfe-',dir=binary.parent);output=Path(temporary.name)
    commands=[];results=[];failures=[];completed=[]
    try:
        policy=NoChildProcess(root,output);stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poisons=[stdlib/'types'/name for name in ['Array.xr','string.xr','i64.xr']]
        for path in poisons:path.write_text('invalid declaration source must not supply compiler values\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        def invoke(name,executable,arguments,environment):
            argv=[str(executable),*arguments]
            result=policy.run(name,argv,environment) if environment is hidden else subprocess.run(argv,cwd=root,env=environment,capture_output=True,timeout=60)
            (output/(name+'.stdout')).write_bytes(result.stdout);(output/(name+'.stderr')).write_bytes(result.stderr)
            commands.append(dict(name=name,argv=argv,exit_code=result.returncode,CLI_child_processes_denied=policy.supported and environment is hidden,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n',encoding='utf-8');return result
        def require(name,passed,result):
            row=dict(name=name,passed=passed,exit_code=result.returncode,stdout_hex=result.stdout.hex(),stderr=result.stderr.decode('utf-8'))
            results.append(row)
            if not passed:failures.append(row)
            return passed
        for name,module,body,expected in cases:
            (output/(name+'-values.xr')).write_text(module,encoding='utf-8');entry=output/(name+'.xr');entry.write_text(body,encoding='utf-8')
            result=invoke(name+'-vm',binary,['run',str(entry)],hidden)
            if not require(name+'-vm',(result.returncode,result.stdout,result.stderr)==(0,expected,b''),result):continue
            generated=output/(name+'.c');result=invoke(name+'-c',binary,['build','--c-only',str(entry),'-o',str(generated)],env)
            if not require(name+'-c',result.returncode==0 and not result.stderr and generated.is_file() and b'({' not in generated.read_bytes(),result):continue
            native=output/(name+'.exe');result=invoke(name+'-build',binary,['build',str(entry),'-o',str(native)],env)
            if not require(name+'-build',result.returncode==0 and not result.stderr,result):continue
            result=invoke(name+'-native',native,[],hidden)
            if require(name+'-native',(result.returncode,result.stdout,result.stderr)==(0,expected,b''),result):completed.append((name,expected))
        for path in poisons:path.unlink()
        for name,expected in completed:
            result=invoke(name+'-source-absent',binary,['run',str(output/(name+'.xr'))],hidden)
            require(name+'-source-absent',(result.returncode,result.stdout,result.stderr)==(0,expected,b''),result)
        negative=negative_cases if negative_cases is not None else [
            ('class','class Box{value:i8;init(value){this.value=value}}\nfn make()->Box{return Box(7)}\nconst p=comptime make()\nprint(p.value)\n',b'comptime result must belong to an admitted closed value family'),
            ('io','struct Pair{value:i8}\nfn make()->Pair{print(1);return Pair{value:7}}\nconst p=comptime make()\nprint(p.value)\n',b'admitted deterministic scalar direct-call closure'),
            ('private-read','import {make} from "./private-result-values"\nconst p=make()\nprint(p.value)\n',b'field access is not permitted'),
            ('private-import','import {Secret} from "./private-inside-values"\nprint(7)\n',b'import requires an exported declaration')]
        for name,body,message in negative:
            entry=output/(name+'.xr');entry.write_text(body,encoding='utf-8');result=invoke(name,binary,['run',str(entry)],hidden)
            require(name,result.returncode==1 and not result.stdout and message in result.stderr,result)
        summary=dict(status=family+'_REQUIRED_CASES_FAIL' if failures else family+'_REQUIRED_CASES_PASS',commands=len(commands),
            required_positive_cases=len(cases),positive_cases_completed=[name for name,_ in completed],all_positive_cases_required=True,
            case_results=results,failures=failures,expected_full_commands=len(cases)*5+len(negative),
            case_semantics=case_semantics or {name:'compiler_constant' for name,_,_,_ in cases},
            source_absence_verified_for_completed_cases=True,source_poisoned=True,Windows_absolute_child_denial=policy.control,
            foreign_aggregate_inputs=False,library_generic_templates=False,library_private_var_opened=False,complete_standard_library=False)
        (output/'summary.json').write_text(json.dumps(summary,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
        print(json.dumps(dict(status=summary['status'],commands=len(commands),required_positive_cases=len(cases),completed=summary['positive_cases_completed'],failures=failures),ensure_ascii=False),flush=True)
        return 1 if failures else 0
    finally:
        if temporary:temporary.cleanup()


if __name__=='__main__':raise SystemExit(main())
