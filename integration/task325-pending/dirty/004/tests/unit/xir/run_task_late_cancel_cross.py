from pathlib import Path
import argparse,ctypes,hashlib,json,subprocess,time,os
from ctypes import wintypes

# Preserve every inherited failure role before mapping the current layout.
LEGACY_PLAN=[(5,0,17,72),(5,0,18,16),(5,0,19,72),(5,0,20,40),(5,0,21,40),(5,1,27,72),(5,1,28,16),(5,1,29,72),(5,1,30,40),(5,1,31,40),
      (6,0,19,72),(6,0,20,40),(6,0,21,40),(6,1,29,72),(6,1,30,40),(6,1,31,40),
      (7,0,19,72),(7,0,20,40),(7,0,21,40),(7,1,29,72),(7,1,30,40),(7,1,31,40),
      (8,0,20,40),(8,0,21,40),(8,1,30,40),(8,1,31,40),(9,0,20,40),(9,0,21,40),(9,1,30,40),(9,1,31,40),
      (10,0,21,40),(10,1,31,40),(11,0,21,40),(11,1,31,40)]
# String/header and enum/header requests grew with the owned Domain prefix.
# First admission frames became local; their work/scratch guards remain below.
PLAN=[(5,0,17,104),(5,0,18,16),(5,0,19,104),(5,1,25,104),(5,1,26,16),(5,1,27,104),
      (6,0,19,104),(6,1,27,104),(7,0,19,104),(7,1,27,104)]
ROLE_MAP=[]
for prefix in range(5,12):
    for instance in range(2):
        old=[row for row in LEGACY_PLAN if row[:2]==(prefix,instance)]
        allocated=[row for row in PLAN if row[:2]==(prefix,instance)]
        index=0
        scratch=[row for row in old if row[3]==40]
        for row in old:
            if row[3]!=40:
                current=allocated[index];index+=1
                assert current[3]==(104 if row[3]==72 else row[3])
                ROLE_MAP.append({'legacy':row,'current_allocation':current})
            else:
                role=2 if len(scratch)==2 and row==scratch[0] else 1
                ROLE_MAP.append({'legacy':row,'guard':{'prefix':prefix,'instance':instance,'role':role}})
        assert index==len(allocated)
assert len(ROLE_MAP)==34 and sum('guard' in row for row in ROLE_MAP)==24
LIMITS=[67108864,67108864,128000000]
ap=argparse.ArgumentParser();ap.add_argument('--exe',required=True);ap.add_argument('--output',required=True)
ap.add_argument('--phase',choices=['normal','full'],default='normal');a=ap.parse_args()
base=Path(a.output);base.mkdir(parents=True,exist_ok=True)
O=base/('run-'+str(time.time_ns())+'-'+str(os.getpid()));O.mkdir();exe=Path(a.exe).resolve()
print('evidence',O,flush=True)
h=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest();pe=h(exe);rows=[];deadline=time.monotonic()+60
k=ctypes.WinDLL('kernel32',use_last_error=True)
class FT(ctypes.Structure):_fields_=[('lo',wintypes.DWORD),('hi',wintypes.DWORD)]
k.GetProcessTimes.argtypes=[wintypes.HANDLE,*([ctypes.POINTER(FT)]*4)];k.GetProcessTimes.restype=wintypes.BOOL
def append(name,row):
    with (O/name).open('a',encoding='utf8') as f:f.write(json.dumps(row,separators=(',',':'))+'\n')
def run(tag,args):
    argv=[str(exe),*map(str,args)];log=O/(tag+'.log');start=time.perf_counter()
    with log.open('wb') as out:
        p=subprocess.Popen(argv,stdout=out,stderr=subprocess.STDOUT);ts=[FT() for _ in range(4)]
        assert k.GetProcessTimes(wintypes.HANDLE(int(p._handle)),*[ctypes.byref(t) for t in ts]);birth=(ts[0].hi<<32)|ts[0].lo
        append('births.jsonl',{'tag':tag,'argv':argv,'pid':p.pid,'creation_filetime':birth})
        timed_out=False
        try:code=p.wait(timeout=max(0.001,min(60,deadline-time.monotonic())))
        except subprocess.TimeoutExpired:timed_out=True;p.kill();code=p.wait()
        assert k.GetProcessTimes(wintypes.HANDLE(int(p._handle)),*[ctypes.byref(t) for t in ts]);end=(ts[1].hi<<32)|ts[1].lo
    row={'tag':tag,'argv':argv,'pid':p.pid,'creation_filetime':birth,'exit_filetime':end,'exit':code,'timed_out':timed_out,'seconds':time.perf_counter()-start,'raw_sha256':h(log),'facts':{},'malformed':[]};rows.append(row)
    try:
        for number,line in enumerate(log.read_text(encoding='utf8').splitlines(),1):
            if line.startswith(('CONTROL_','CENSUS_','LATE_','CROSS_')):
                prefix,body=line.split(' ',1)
                try:row['facts'].setdefault(prefix,[]).append(json.loads(body))
                except json.JSONDecodeError as e:row['malformed'].append({'line':number,'raw':line,'error':str(e)})
        assert not row['malformed'] and not timed_out and code==0,row
        assert len(row['facts']['CENSUS_FINAL'])==1
        final=row['facts']['CENSUS_FINAL'][0];assert final['physical']==[0,0] and final['semantic']==73
<<<<<<< HEAD
=======
        assert row['facts']['CENSUS_LAYOUT']==[{'string':104,'enum_one_field':104,'admission_frame':40,'call_result':72}]
>>>>>>> ec41df298c2bc6370d94c5388706bef85cb6fb12
        assert len(row['facts']['CROSS_RESULT'])==2
        assert all(x['epoch']==1 and x['limits']==LIMITS for x in row['facts']['CROSS_RESULT'])
        return row
    finally:append('terminals.jsonl',row)
def copy_assert(row,expected):
    copies=row['facts'].get('CONTROL_COPY',[])
    assert len(copies)==(4 if expected else 0)
    if copies:
        assert all(x['size']==72 and x['delegations']==1 and x['value_status']==0 and x['status']==x['cached']==expected[x['instance']] for x in copies)
def admission_assert(row,prefix,guards=False):
    facts=row['facts']
    public=facts.get('CONTROL_ADMISSION',[]);private=facts.get('CONTROL_CALL_ADMISSION',[])
    assert [x['instance'] for x in public]==([0,1] if isinstance(prefix,int) and 5<=prefix<=11 else [])
    assert [x['instance'] for x in private]==([0,1] if isinstance(prefix,int) and 5<=prefix<=9 else [])
    assert all(x['type']==256 and x['status']==0 and x['allocations']==0 and
               x['work']==[1000000,999998] and x['scratch']==[16777216,16777216] for x in public)
    assert all(x['type']==256 and x['status']==2 for x in private)
    controls=facts.get('CONTROL_GUARD',[])
    want=[(x['guard']['instance'],x['guard']['role'],control) for x in ROLE_MAP
          if 'guard' in x and x['guard']['prefix']==prefix for control in range(3)] if guards else []
    assert sorted((x['instance'],x['role'],x['control']) for x in controls)==sorted(want)
    assert all(x['status']==([0,4,4][x['control']]) and x['work_fee']==[3,2,1][x['control']] and
               x['scratch']==[40,40,39][x['control']] and x['allocations']==0 for x in controls)
    assert facts['CENSUS_FINAL'][0]['guards']==len(want)

try:
    measured=[]
    for prefix in ['baseline',*range(14)]:
        row=run('normal-'+str(prefix),[] if prefix=='baseline' else [prefix]);f=row['facts']
        assert f['CENSUS_FINAL'][0]['hits']==0 and not f.get('CROSS_HIT')
        results=f['CROSS_RESULT']
        if prefix in ['baseline',13]:assert all((x['root'],x['task'],x['first'],x['free'])==(1,2,0,0) for x in results)
        else:assert all((x['root'],x['task'],x['first'],x['free'])==(4,0 if prefix<5 else 2,0,0) for x in results)
        copy_assert(row,None if isinstance(prefix,int) and prefix<5 else [2,2])
        if prefix==13:assert len(f['CENSUS_TERMINAL_CANCEL'])==2 and all(x['status']==9 for x in f['CENSUS_TERMINAL_CANCEL'])
        elif prefix!='baseline':assert len(f['CENSUS_CANCEL'])==2 and all(x['status']==18 for x in f['CENSUS_CANCEL'])
        late=[x for x in f.get('CONTROL_ALLOC',[]) if x['after_cancel'] and x['phase']==3]
        if prefix!='baseline':measured.extend((prefix,x['instance'],x['ordinal'],x['bytes']) for x in late)
        admission_assert(row,prefix)
        print(row['tag'],'PASS',flush=True)
<<<<<<< HEAD
    # Stop before injecting if the actual current topology differs at all.
=======
    # Stop before injecting if the current Checked topology differs at all.
>>>>>>> ec41df298c2bc6370d94c5388706bef85cb6fb12
    assert measured==PLAN,{'actual':measured,'independent_plan':PLAN}
    if a.phase=='full':
        for prefix in range(5,12):
            row=run('guards-'+str(prefix),[prefix,'--guards'])
            assert all((x['root'],x['task'],x['first'],x['free'])==(4,2,0,0) for x in row['facts']['CROSS_RESULT'])
            copy_assert(row,[2,2]);admission_assert(row,prefix,True)
            print(row['tag'],'PASS',flush=True)
        for index,(prefix,instance,ordinal,size) in enumerate(PLAN):
            row=run('oom-'+str(index),[prefix,ordinal,instance]);f=row['facts']
            assert f['CENSUS_FINAL'][0]['hits']==1 and len(f['CROSS_HIT'])==1
            hit=f['CROSS_HIT'][0];assert (hit['instance'],hit['ordinal'],hit['bytes'],hit['phase'],hit['after_cancel'])==(instance,ordinal,size,3,1)
            assert any(x['instance']==instance and x['ordinal']==ordinal and x['bytes']==size and x['after_cancel']==1 and x['phase']==3 for x in f['CONTROL_ALLOC'])
            for result in f['CROSS_RESULT']:
                want=(6,6,6,6) if result['instance']==instance else (4,2,0,0)
                assert (result['root'],result['task'],result['first'],result['free'])==want
            copy_assert(row,[6 if i==instance else 2 for i in range(2)])
            print(row['tag'],'PASS',flush=True)
    assert h(exe)==pe
    (O/'receipt.json').write_text(json.dumps({'status':'NORMAL_15_CROSS_10_GUARDS_72_PASS' if a.phase=='full' else 'NORMAL_15_PASS_CROSS_NOT_RUN','PE_before_after':pe,'phase':a.phase,'actual_normal':15,'actual_cross':10 if a.phase=='full' else 0,'actual_guard_controls':72 if a.phase=='full' else 0,'legacy_failure_roles':34,'role_mapping':ROLE_MAP,'parent_deadline':60,'per_child_timeout':60,'jobs':1,'limits':LIMITS,'plan':PLAN,'all_handles_terminal':True},indent=2)+'\n')
finally:(O/'processes.json').write_text(json.dumps(rows,indent=2)+'\n')
