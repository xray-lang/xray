from pathlib import Path
import argparse,ctypes,hashlib,json,subprocess,time,os
from ctypes import wintypes
ap=argparse.ArgumentParser();ap.add_argument('--exe',required=True);ap.add_argument('--output',required=True);a=ap.parse_args()
base=Path(a.output);base.mkdir(parents=True,exist_ok=True)
O=base/('run-'+str(time.time_ns())+'-'+str(os.getpid()));O.mkdir(exist_ok=False);exe=Path(a.exe).resolve()
print('evidence',str(O),flush=True)
h=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest();pe=h(exe);rows=[]
k=ctypes.WinDLL('kernel32',use_last_error=True)
class FT(ctypes.Structure):_fields_=[('lo',wintypes.DWORD),('hi',wintypes.DWORD)]
k.GetProcessTimes.argtypes=[wintypes.HANDLE,*([ctypes.POINTER(FT)]*4)];k.GetProcessTimes.restype=wintypes.BOOL
try:
 for mode in range(4):
  args=[str(exe),str(mode)];log=O/f'{mode}.log';start=time.perf_counter()
  with log.open('wb') as out:
   p=subprocess.Popen(args,stdout=out,stderr=subprocess.STDOUT);ts=[FT() for _ in range(4)]
   assert k.GetProcessTimes(wintypes.HANDLE(int(p._handle)),*[ctypes.byref(t) for t in ts]);birth=(ts[0].hi<<32)|ts[0].lo
   (O/f'{mode}-birth.json').write_text(json.dumps({'argv':args,'pid':p.pid,'creation_filetime':birth})+'\n')
   timed_out=False
   try:code=p.wait(timeout=60)
   except subprocess.TimeoutExpired:timed_out=True;p.kill();code=p.wait()
   assert k.GetProcessTimes(wintypes.HANDLE(int(p._handle)),*[ctypes.byref(t) for t in ts]);end=(ts[1].hi<<32)|ts[1].lo
  facts={};row={'argv':args,'pid':p.pid,'creation_filetime':birth,'exit_filetime':end,'exit':code,'seconds':time.perf_counter()-start,'timed_out':timed_out,'raw_sha256':h(log),'facts':facts};rows.append(row)
  for line in log.read_text(encoding='utf8').splitlines():
   if line.startswith('CONTROL_'):
    prefix,body=line.split(' ',1);facts.setdefault(prefix,[]).append(json.loads(body))
  assert not timed_out and code==0,row
  assert facts['CONTROL_FINAL'][0]['physical']==[0,0]
  assert facts['CONTROL_FINAL'][0]['injected']==facts['CONTROL_FINAL'][0]['denials']==0
  assert len(facts['CONTROL_GO'])==len(facts['CONTROL_RESULT'])==2 and len(facts['CONTROL_COPY'])==4
  expected=4 if mode in [0,3] else 6 if mode==1 else 5
  assert all(x['status']==x['cached']==expected and x['value_status']==0 and x['delegations']==1 and x['size']==72 for x in facts['CONTROL_COPY'])
  assert all(x['epoch']==1 and x['limits']==[67108864,67108864,128000000] and x['terminal']==expected for x in facts['CONTROL_RESULT'])
  if mode in [0,3]: assert len(facts['CONTROL_CANCEL'])==2 and not facts.get('CONTROL_OUTPUT')
  else: assert len(facts['CONTROL_OUTPUT'])==2 and all(x['status']==mode for x in facts['CONTROL_OUTPUT'])
  # Copy/drop bookkeeping is deliberately separate from cancelled execution malloc.
  late=[x for x in facts.get('CONTROL_ALLOC',[]) if x['after_cancel'] and x['phase'] in [3,4]]
  row['late_cancel_execution_malloc']=late
  print(mode,'PASS','late_cancel_malloc',len(late),flush=True)
 assert h(exe)==pe
 (O/'receipt.json').write_text(json.dumps({'status':'NORMAL_CACHED_FAILURE_COPY_CONTROLS_PASS','PE_before_after':pe,'commands':4,'per_child_timeout':60,'jobs':1,'rows':rows,'cancel_late_full':'NOT_RUN'},indent=2)+'\n')
finally:
 (O/'processes.json').write_text(json.dumps(rows,indent=2)+'\n')
