from pathlib import Path
import hashlib,json,re,subprocess,sys
exe=Path(sys.argv[1]);mode=sys.argv[2];assert mode in ('--normal','--resources','--before')
maximum=(1<<64)-1
def invoke(case,fault):
 r=subprocess.run([str(exe),str(fault),str(case)],capture_output=True,timeout=5)
 assert r.returncode==(1 if mode=='--before' else 0),(r.returncode,r.stderr)
 assert b'AddressSanitizer' not in r.stderr and b'runtime error:' not in r.stderr,r.stderr
 m=re.search(rb'RESOLVE_WIRE mode=(\d+) sites=(\d+) handler_sites=(\d+) injected=(\d+) exit=(\d+) stage=(\d+) status=(\d+) result=(\d+) JSON_and_transport0/0 dispatch_policy_actual=1 cancellation_spy=1',r.stderr);assert m,r.stderr
 stats=list(map(int,m.groups()));wire=[];data=r.stdout
 while data:
  header,sep,rest=data.partition(b'\r\n\r\n');assert sep and re.fullmatch(rb'Content-Length: [0-9]+',header)
  length=int(header.split(b': ')[1]);assert 0<length<=len(rest);wire.append(json.loads(rest[:length]));data=rest[length:]
 assert len(wire)<=1,wire
 for value in wire:assert value['jsonrpc']=='2.0' and value['id']==(2147483647 if case%2 else 'a\0b😀'),value
 if mode=='--before':
  assert fault==0 and case==0 and stats[3:6]==[1,0,0] and wire[0]['result'] is None and 'error' not in wire[0]
  assert b'BUG resolve_clone_OOM_published_success_NULL=1' in r.stderr
 elif fault!=maximum:
  assert stats[3]==1,stats
  if stats[4]:assert not wire,wire
  else:
   assert len(wire)==1 and 'result' not in wire[0] and wire[0]['error']['code'] in (-32603,-32800),wire
 else:
  assert stats[3:5]==[0,0] and len(wire)==1,stats
  code=-32602 if case in (1,2,3,4,5,6,11) else -32800 if case in (8,9) else -32603 if case in (10,12) else None
  if code is None:
   assert 'error' not in wire[0]
   if case==13:assert wire[0]['result'] is None
   else:
    value=wire[0]['result'];assert value['label']==('' if case==7 else 'a\0b😀' if case==14 else 'same'),value
    if case==0:assert value['documentation']=='owned' and value['data']['uri']=='file:///closed.xr'
  else:assert 'result' not in wire[0] and wire[0]['error']['code']==code,wire
 print(json.dumps(dict(case=case,fault=fault,stats=stats,wire=wire),ensure_ascii=True))
 return stats[1]
if mode=='--before':invoke(0,0)
else:
 total=checked=0
 for case in range(15):
  sites=invoke(case,maximum);assert sites;total+=sites
  for fault in range(sites if mode=='--resources' else 1):invoke(case,fault);checked+=1
 print(json.dumps(dict(cases=15,prefixes=total,checked=checked,full_FI=mode=='--resources',image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),public_server=False)))
