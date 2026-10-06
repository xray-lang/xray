from pathlib import Path
import argparse,ctypes,hashlib,json,os,re,subprocess,time
from ctypes import wintypes
SHA=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
FIRST=[13,13,13,13,13,13,4,13,1,13,6,5,8,7,13,6,5,1,13]
FINAL=[13,13,13,13,13,13,4,1,1,1,1,1,8,7,1,1,1,1,1]
# The AWAIT child host fault remains executor shutdown status; the other controls have no unreplaced child failure.
FREE=[0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
OLD_S=[8,8,11,8,8,8,8,10,11,10,10,10,8,8,16,16,16,11,10]
OLD_P=[15,25,20,21,13,9,15,29,18,29,29,29,10,10,40,40,40,18,33]
U=4294967295;Z=18446744073709551615
p=argparse.ArgumentParser();p.add_argument('--mode',choices=['baseline','plan','run'],required=True);p.add_argument('--exe',type=Path);p.add_argument('--binding',type=Path);p.add_argument('--baseline',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--window-approved',action='store_true');p.add_argument('--approve-denominator-change',action='store_true');a=p.parse_args()
assert not a.out.exists();a.out.mkdir(parents=True)
def dump(n,x):(a.out/n).write_text(json.dumps(x,indent=2)+'\n')
if a.mode!='plan':
 assert a.exe and a.binding
 bind=json.loads(a.binding.read_text());assert SHA(a.exe)==bind['executable_sha256']
 def guard():
  current={path:SHA(Path(path)) for path in bind['inputs']};assert current==bind['inputs'];return current
 before=guard();dump('inputs-before.json',before)
 native=ctypes.WinDLL('kernel32',use_last_error=True)
 class FT(ctypes.Structure):_fields_=[('lo',wintypes.DWORD),('hi',wintypes.DWORD)]
 native.GetProcessTimes.argtypes=[wintypes.HANDLE,ctypes.POINTER(FT),ctypes.POINTER(FT),ctypes.POINTER(FT),ctypes.POINTER(FT)];native.GetProcessTimes.restype=wintypes.BOOL
 ticks=lambda f:(int(f.hi)<<32)|int(f.lo)
 me=[FT() for _ in range(4)];native.GetProcessTimes(wintypes.HANDLE(-1),*[ctypes.byref(f) for f in me]);dump('runner-identity.json',{'pid':os.getpid(),'creation_filetime':ticks(me[0]),'exe':str(a.exe),'exe_sha256':SHA(a.exe)})
def command(index,args):
 log=a.out/(str(index).zfill(4)+'.log');argv=[str(a.exe)]+list(map(str,args));start=time.time()
 with log.open('wb') as stream:
  child=subprocess.Popen(argv,cwd=a.exe.parent,stdout=stream,stderr=subprocess.STDOUT)
  times=[FT() for _ in range(4)];assert native.GetProcessTimes(wintypes.HANDLE(int(child._handle)),*[ctypes.byref(f) for f in times]);created=ticks(times[0])
  try:code=child.wait(timeout=60);timedout=False
  except subprocess.TimeoutExpired:child.kill();code=child.wait();timedout=True
  assert native.GetProcessTimes(wintypes.HANDLE(int(child._handle)),*[ctypes.byref(f) for f in times])
 record={'argv':argv,'pid':child.pid,'creation_filetime':created,'exit_filetime':ticks(times[1]),'exit':code,'timeout':timedout,'seconds':time.time()-start,'log_sha256':SHA(log)}
 with (a.out/'processes.jsonl').open('a') as f:f.write(json.dumps(record)+'\n')
 assert code==0 and not timedout,record
 text=log.read_text();rows=[json.loads(s) for s in text.splitlines() if s.startswith('{"id"')];assert len(rows)==1;r=rows[0]
 assert all(r[k]==v for k,v in zip(['id','fail_at','axis','cap','cancel_prefix'],args)) and r['physical_zero']
 assert 'ASSERT/physical0' in text and 'final physical=0/0' in text
 event=re.search(r'ORACLE id(\d+) hit(\d+) phase(\d+) step(\d+) reason(\d+) restart(\d+) outputcalls(\d+)',text);assert event
 event=list(map(int,event.groups()[1:]))
 denial=re.search(r'BUDGET rejected(\d+) firstPhase(\d+) ASSERT',text);assert denial
 ctl=re.search(r'CONTROL cancelSeen(\d+) cancelAccepted(\d+) cancelStatus(\d+) incomplete(\d+) frontierCount(\d+) ASSERT',text);assert ctl
 hosts=[list(map(int,m.groups())) for m in re.finditer(r'HOST_AFTER_CANCEL provider(\d+) accepted(\d+) phase(\d+) step(\d+) drivercancel(\d+) cleanup(\d+) prior(\d+) epoch(\d+) ASSERT',text)]
 for provider,accepted,phase,step,cancel,cleanup,prior,epoch in hosts:
  assert provider in [1,2,3] and accepted=={1:6,2:5,3:13}[provider]
  assert phase in [4,6] and (not cancel or cleanup) and prior in [0,4] and epoch>0
 protocols=[list(map(int,m.groups())) for m in re.finditer(r'PROTOCOL_AFTER_CANCEL provider(\d+) accepted(\d+) phase(\d+) step(\d+) drivercancel(\d+) cleanup(\d+) prior(\d+) epoch(\d+) ASSERT',text)]
 for provider,accepted,phase,step,cancel,cleanup,prior,epoch in protocols:
  assert provider in [5,99] and accepted=={5:8,99:7}[provider]
  assert phase in [4,6] and (not cancel or cleanup) and prior==0 and epoch>0
 allocations=[list(map(int,m.groups())) for m in re.finditer(r'ALLOC ordinal(\d+) bytes(\d+) kind(\d+) phase(\d+) step(\d+) epoch(\d+)',text)]
 assert len(allocations)==r['runtime_malloc_sites'] and [v[0] for v in allocations]==list(range(len(allocations)))
 return {'record':record,'row':r,'event':event,'denial':list(map(int,denial.groups())),'control':list(map(int,ctl.groups())),'allocations':allocations,'host_after_cancel':hosts,'protocol_after_cancel':protocols}
def plan(base):
 rows=[]
 for b in base:
  id=b['row']['id'];r=b['row']
  for k in range(r['runtime_malloc_sites']):rows.append({'kind':'OOM','args':[id,k,U,0,U]})
  for axis,key in enumerate(['requested_value','requested_call','work']):
   for minus in [0,1]:rows.append({'kind':'axis','minus':minus,'args':[id,Z,axis,r[key]-minus,U]})
  for k in range(r['poll_prefix_steps']+1):rows.append({'kind':'cancel','args':[id,Z,U,0,k]})
 return rows
results=[];active=None
try:
 if a.mode=='baseline':
  for id in range(19):
   x=command(id,[id,Z,U,0,U]);r=x['row'];assert r['created']==0 and r['started']==0 and r['first_status']==FIRST[id] and r['final_status']==FINAL[id] and r['free_status']==FREE[id]
   assert x['denial'][0]==0
   assert r['requested_value']<=67108864 and r['requested_call']<=67108864 and r['work']<=128000000
   results.append(x)
  dump('baselines.json',{'status':'ACTUAL_FRESH_TYPED_BASELINE_NOT_FULL_SCAN','operations':results,'binding_sha256':SHA(a.binding)})
 else:
  assert a.baseline;baselines=json.loads(a.baseline.read_text());base=baselines['operations'];assert len(base)==19
  rows=plan(base);totals={k:sum(c['kind']==k for c in rows) for k in ['OOM','axis','cancel']};assert totals['axis']==114
  drift=totals!={'OOM':195,'axis':114,'cancel':462};dump('plan.json',{'status':'PREPARED_NOT_EXECUTED','commands':rows,'counts':totals,'old88d195_and771_not_inherited':True,'denominator_changed':drift})
  if a.mode=='plan':raise SystemExit(0)
  assert a.window_approved and (not drift or a.approve_denominator_change)
  assert baselines['binding_sha256']==SHA(a.binding),'Never reuse older sole fees/sites'
  for index,c in enumerate(rows):
   active=c;x=command(index,c['args']);r=x['row'];id=r['id'];b=base[id]['row'];hit,phase,step,reason,restart,calls=x['event'];reject,denialphase=x['denial'];seen,accepted,cancelstatus,incomplete,frontiers=x['control']
   if c['kind']=='OOM':
    assert hit==1 and r['runtime_malloc_sites']>c['args'][1]
    if phase==1:assert r['created']==6 and r['started']==9 and not calls
    elif phase==2:assert r['created']==0 and r['started']==9
    elif phase==3:assert r['created']==0 and r['started']==6 and not calls
    elif phase==5:assert restart==6 and r['final_status']==r['first_status']
    elif phase in [4,6]:assert reason in [4,5,6,7,8,13] and r['first_status' if phase==4 else 'final_status']==reason
    else:assert phase in [7,8] and (r['free_status'] in [4,5,6,7,8,13] or r['final_status'] in [4,5,6,7,8,13])
   elif c['kind']=='axis':
    assert not hit
    key=['requested_value','requested_call','work'][c['args'][2]];assert r[key]<=c['args'][3]
    if not c['minus']:
     assert reject==0
     for k in ['created','started','first_status','final_status','free_status','requested_value','requested_call','work','runtime_malloc_sites','poll_prefix_steps']:assert r[k]==b[k]
    else:assert reject>0 and denialphase in range(1,9)
   else:
    assert not hit and not reject and seen==1 and cancelstatus in [0,9,18] and accepted==(cancelstatus==18)
    assert r['created']==0 and r['started']==0
    hosts=x['host_after_cancel']+x['protocol_after_cancel'];assert len(hosts)<=1
    if hosts and hosts[0][2]==4:assert r['first_status']==hosts[0][1]
    else:assert r['first_status'] in [b['first_status'],4]
    if hosts and (hosts[0][2]==6 or (hosts[0][2]==4 and restart!=0)):assert r['final_status']==hosts[0][1]
    else:assert r['final_status'] in [b['first_status'],b['final_status'],4]
    if c['args'][4]==b['poll_prefix_steps']:assert not accepted and r['first_status']==b['first_status'] and r['final_status']==b['final_status']
    # An LCS audit distinguishes new cancellation allocations from mere total drift.
    # Matching keys ignore step; phase/epoch/kind/bytes remain exact recorded facts.
    old=base[id]['allocations'];new=x['allocations'];key=lambda e:(e[1],e[2],e[3],e[5]);table=[[0]*(len(new)+1) for _ in range(len(old)+1)]
    for i in range(len(old)-1,-1,-1):
     for j in range(len(new)-1,-1,-1):table[i][j]=1+table[i+1][j+1] if key(old[i])==key(new[j]) else max(table[i+1][j],table[i][j+1])
    i=j=0;matched=set()
    while i<len(old) and j<len(new):
     if key(old[i])==key(new[j]):matched.add(j);i+=1;j+=1
     elif table[i+1][j]>=table[i][j+1]:i+=1
     else:j+=1
    x['cancel_extra_allocation_candidates']=[e for j,e in enumerate(new) if j not in matched]
    x['cancel_cross_OOM']='NOT_SCANNED; additional runtime allocation candidates need separate permission, equal-size event matching is not object identity proof'
   x['case']=c;x['status']='ASSERT_PASS';results.append(x);dump('progress.json',{'status':'RUNNING','commands':results})
  dump('receipt.json',{'status':'ACTUAL_FULL_ASSERT_PASS','counts':totals,'commands':results,'binding_sha256':SHA(a.binding),'cancel_cross_OOM':'OPEN; no cross-fault expansion'})
except Exception as error:
 dump('first-failure.json',{'status':'FAIL_STOPPED','detail':str(error),'commands_before_failure':results,'active_case':active});raise
finally:
 if a.mode!='plan':
  after=guard();assert before==after;dump('inputs-after.json',after)
