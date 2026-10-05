"""Run every named source-owner case and physical fault exactly once in a bounded Job."""
from __future__ import annotations
import argparse,ctypes,hashlib,json,os,subprocess,sys,time,uuid
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'lib'))
from xraytest.windows_owned_job import establish_job
CASES=['coalloc','coalloc_early','call_storage','region_output','plan_context','conversion_budget',
 'generic_method_snapshot','implementation_snapshot','constraint_plain','constraint_mixed','interface_snapshot',
 'enum_snapshot_empty','enum_snapshot_payload','nominal_substitution','nominal_snapshot_nodes','nominal_snapshot_declarations',
 'constructed_snapshot','root_source','array_source','method_source','requirement_receiver','requirement_generic',
 'enum_source','class_source','class_array_source','iteration_source','inference_unit','inference_unit_noncall',
 'inference_dead_catch','inference_interface','inference_ordinary','inference_unit_result','resolver_faults']
PURE={0,1,2,3,4,5,32};WORKERS=8;TIMEOUT=600
def checked(value,message):
    if not value:raise RuntimeError(message)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def records(path):
    checked(path.is_file() and path.stat().st_size<=16*1024*1024,'Missing/oversize actual report')
    return [json.loads(line)for line in path.read_text(encoding='utf-8').splitlines()]
def zero(row):
    checked(type(row.get('compiler_blocks'))is int and type(row.get('compiler_bytes'))is int and
            row['compiler_blocks']==row['compiler_bytes']==0,'Residual compiler ownership')
def complete(rows,mode,index):
    expected={'kind':'complete','mode':mode,'index':index,'workers':WORKERS,'compiler_blocks':0,'compiler_bytes':0}
    checked(rows and rows[-1]==expected,'Missing exact process completion')
def case_rows(rows,ids):
    found={}
    for row in rows:
        if row['kind']!='case':continue
        cid=row.get('case');checked(type(cid)is int and cid in ids and cid not in found,'Duplicate/unknown original case')
        checked(row['name']==CASES[cid] and (row['allocated_cap'],row['live_cap'],row['work_cap'])==(67108864,8388608,128000000),'Case/whole-operation caps changed')
        checked(type(row['sites'])is int and 0<=row['sites']<=67108864 and ((row['sites']==0)==(cid in PURE)),'Invalid denominator')
        zero(row);found[cid]=row
    checked(set(found)==set(ids),'Missing original case');return found
def job_processes(job):
    class Accounting(ctypes.Structure):
        _fields_=[('user',ctypes.c_longlong),('kernel',ctypes.c_longlong),('period_user',ctypes.c_longlong),
                  ('period_kernel',ctypes.c_longlong),('faults',ctypes.c_uint32),('total',ctypes.c_uint32),
                  ('active',ctypes.c_uint32),('terminated',ctypes.c_uint32)]
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.QueryInformationJobObject.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p]
    kernel.QueryInformationJobObject.restype=ctypes.c_int
    state=Accounting();checked(kernel.QueryInformationJobObject(job,1,ctypes.byref(state),ctypes.sizeof(state),None),'Cannot observe owned Job drain')
    return state.active
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,required=True)
    parser.add_argument('--reports-root',type=Path,required=True)
    parser.add_argument('--source-root',type=Path,required=True)
    args=parser.parse_args();checked(os.name=='nt','Owned Windows parent required')
    started=time.monotonic();deadline=started+TIMEOUT
    executable=args.executable.resolve(strict=True);source=args.source_root.resolve(strict=True)
    out=args.reports_root/(time.strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex);out.mkdir(parents=True)
    inputs={str(p):sha(p)for p in source.rglob('*')if p.is_file() and p.suffix in
            {'.c','.h','.def','.py','.cmake','.xr','.toml','.json'} and '__pycache__' not in p.parts}
    manifest={'qualified':False,'timeout':TIMEOUT,'workers':WORKERS,'case_names':CASES,'original_cases':len(CASES),
              'executable':str(executable),'executable_sha256':sha(executable),'inputs':inputs,'commands':[]}
    active=[];job=None
    def publish():(out/'qualification.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    def launch(name,arguments):
        checked(time.monotonic()<deadline,'Original 600-second overall deadline exhausted')
        command=[str(executable),*arguments,str(out/(name+'.jsonl'))]
        with(out/(name+'.log')).open('xb')as log:
            child=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,close_fds=True)
        row={'name':name,'argv':command,'pid':child.pid};active.append((child,row));manifest['commands'].append(row);publish()
    def drain():
        while active:
            checked(time.monotonic()<deadline,'Original 600-second overall deadline exhausted')
            for child,row in list(active):
                status=child.poll()
                if status is not None:
                    active.remove((child,row));row['exit_code']=status;publish()
                    checked(status==0,f"{row['name']} exited {status}; see {out/(row['name']+'.log')}")
            if active:time.sleep(.01)
    try:
        job=establish_job();manifest['windows_job']=job;publish()
        launch('baseline',['--allocation-baseline']);drain()
        rows=records(out/'baseline.jsonl');complete(rows,1,0);normal=case_rows(rows,range(len(CASES)))
        checked(not any(r['kind']=='point'for r in rows),'Full fault sweep duplicated in baseline')
        pure={(0,'map_snapshot',i)for i in range(3)}|{(0,'aligned_snapshot',i)for i in range(4)}|{
            (2,'call_storage',0),(3,'region_output',0),(3,'region_output',1),(5,'conversion_recipe',0)}
        observed=set()
        for row in rows:
            if row['kind']!='pure_point':continue
            key=(row['case'],row['role'],row['ordinal']);checked(key in pure and key not in observed,'Missing/duplicate private original fault')
            checked(row['status']==7 and row['injected']is True and type(row['attempts'])is int and
                    type(row['fail_at'])is int and row['attempts']>row['fail_at'],'Private physical failure not reached');zero(row);observed.add(key)
        checked(observed==pure,'Private fault denominator incomplete')
        counts=','.join(str(normal[i]['sites'])for i in range(len(CASES)))
        manifest['baseline']=normal;manifest['pure_faults']=sorted(observed);publish()
        for index in range(WORKERS):launch(f'shard-{index}',['--allocation-shard',str(index),counts])
        drain();unions={i:set()for i in range(len(CASES))if i not in PURE}
        for index in range(WORKERS):
            rows=records(out/f'shard-{index}.jsonl');complete(rows,2,index);part=case_rows(rows,unions)
            checked(not any(r['kind']=='pure_point'for r in rows),'Private original cases duplicated by worker')
            per={i:[]for i in unions}
            for row in rows:
                if row['kind']!='point':continue
                cid=row.get('case');ordinal=row.get('ordinal');checked(cid in unions and type(ordinal)is int and
                    0<=ordinal<normal[cid]['sites']and ordinal%WORKERS==index and ordinal not in unions[cid],'Invalid/duplicate/wrong partition')
                checked(row['status']==7 and row['injected']is True and row['empty_output']is True and
                    type(row['attempts'])is int and row['attempts']>ordinal,'Actual OOM/output obligation missing')
                zero(row);unions[cid].add(ordinal);per[cid].append(ordinal)
            for cid in unions:
                expected=list(range(index,normal[cid]['sites'],WORKERS))
                checked(per[cid]==expected and part[cid]['sites']==normal[cid]['sites'] and
                        part[cid]['points']==len(expected),'Ordered family shard is incomplete')
        checked(all(unions[i]==set(range(normal[i]['sites']))for i in unions),'Exact original union incomplete')
        manifest['completed_ordinals']={i:sorted(unions[i])for i in unions}
        checked(not active and time.monotonic()<deadline,'Processes/deadline not complete')
        checked(sha(executable)==manifest['executable_sha256'] and all(sha(Path(p))==h for p,h in inputs.items()),'Actual inputs changed')
        # All child outputs are direct files, so wait observes exit and closes
        # their writers without a bounded pipe that could strand shutdown.
        state=job_processes(job);checked(state==1,'Unobserved descendants remain');manifest['job_active_processes']=state
        manifest['qualified']=True;manifest['seconds']=time.monotonic()-started;publish()
        print(f"Source allocations: 33 original cases, 11 private faults, {sum(len(v)for v in unions.values())} exact whole-operation faults, physical0; {out}")
        return 0
    except Exception as error:
        manifest['failure']=str(error);manifest['seconds']=time.monotonic()-started;publish()
        print(f'Source allocation FAIL: {error}; {out}',file=sys.stderr);return 1
    finally:
        for child,_ in active:
            if child.poll()is None:child.kill()
        for child,row in active:
            row['exit_code']=child.wait();row['terminated_on_failure']=True
        if active:
            manifest['failure_job_active_processes']=job_processes(job);publish()
            checked(manifest['failure_job_active_processes']==1,'Failure cleanup did not observe all descendant exits')
        # Process exit closes the sole non-inherited Job handle; do not close a
        # Job containing the live parent. CTest parent termination has the same rule.
if __name__=='__main__':raise SystemExit(main())
