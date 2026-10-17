from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile,time
exe,stdlib=map(lambda p:Path(p).resolve(),sys.argv[1:3]);resource='--resources' in sys.argv[3:];deadline=time.monotonic()+(290 if resource else 55)
image=hashlib.sha256(exe.read_bytes()).hexdigest();env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib)}
metric=re.compile(rb'PUBLICATION mode=(\d+) source=(\d+) json=(\d+) app=(\d+) source_hit=(\d+) json_hit=(\d+) app_hit=(\d+) failed=(\d+) write_calls=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) physical0/0 JSON0/0 app0/0')
def unpack(data):
 result=[]
 while data:
  header,separator,tail=data.partition(b'\r\n\r\n');assert separator and re.fullmatch(rb'Content-Length: [0-9]+',header)
  size=int(header.split(b': ')[1]);assert size and size<=len(tail)
  value=json.loads(tail[:size]);assert value['jsonrpc']=='2.0' and value['method']=='textDocument/publishDiagnostics'
  result.append(value['params']);data=tail[size:]
 return result
with tempfile.TemporaryDirectory(prefix='publication-😀-#-',dir=exe.parent) as folder:
 root=Path(folder).resolve();assert root.is_relative_to(exe.parent);commands={};original={}
 for mode in range(12):
  case=root/str(mode);case.mkdir();paths=[case/n for n in ['root.xr','lib.xr','peer.xr']]
  (case/'xray.toml').write_bytes(b'[project]\nname="publication"\nmain="root.xr"\n')
  paths[1].write_bytes('/* 😀 */ var *** = ;\n'.encode())
  (case/'semantic.xr').write_bytes(b'export fn value()->i64{return missing}\n')
  commands[mode]=[str(exe),*(p.as_uri() for p in paths),str(mode)];original.update({p:p.read_bytes() for p in case.iterdir()})
 def invoke(mode,*args):
  remaining=deadline-time.monotonic();assert remaining>0
  p=subprocess.run([*commands[mode],*map(str,args)],input=b'',capture_output=True,env=env,timeout=remaining)
  if p.returncode:sys.stderr.write(p.stderr.decode('utf-8','replace'));raise AssertionError((mode,args,p.returncode))
  assert b'AddressSanitizer' not in p.stderr and b'runtime error:' not in p.stderr
  m=metric.search(p.stderr);assert m,p.stderr
  row=dict(mode=int(m[1]),source=int(m[2]),json=int(m[3]),app=int(m[4]),source_hit=int(m[5]),json_hit=int(m[6]),app_hit=int(m[7]),failed=int(m[8]),write_calls=int(m[9]),allocated=int(m[10]),peak=int(m[11]),work=int(m[12]))
  return row,unpack(p.stdout)
 def baseline(mode):
  row,values=invoke(mode);root_uri,lib_uri,peer_uri=commands[mode][1:4]
  counts=[1,3,3,4,2,1,0,2,2,2,3,2];assert len(values)==counts[mode],(mode,values)
  if mode!=6:
   first=values[0];assert first['uri']==lib_uri and first['version']==(7 if mode==4 else 8 if mode==5 else None)
   diag=first['diagnostics'][0];end=14 if mode==5 else 13
   assert diag['message']=='expected variable name' and diag['range']==dict(start=dict(line=0,character=13),end=dict(line=0,character=end))
  for value in values[1:]:
   if mode in (2,11) and value is values[1]:assert value['uri']==lib_uri and value['version'] is None and value['diagnostics'];continue
   assert not value['diagnostics']
   if mode==1 and value is values[-1]:assert value['uri']==root_uri and value['version']==2
   else:assert 'version' not in value
  if mode==3:assert sum(v['uri']==lib_uri and not v['diagnostics'] for v in values)==1
  assert row['failed']==(mode==7);return row
 try:
  baselines=[baseline(i) for i in ([0,1,8,9,10,11] if resource else range(12))];checked=dict(source=0,json=0,app=0);axes=0
  plans=[(row,d,index) for row in baselines if row['mode'] in [0,1,8,9,10,11] for d in checked for index in (range(row[d]) if resource else ([0] if row[d] else []))]
  def fault(plan):
   row,domain,index=plan;value,wire=invoke(row['mode'],'--fault',domain,index)
   assert value[domain+'_hit'] and value['failed']
   if row['mode']==10:assert 1<=len(wire)<=2
   else:assert len(wire)==(0 if not row['mode'] else 1)
   return domain
  with ThreadPoolExecutor(max_workers=8) as pool:
   for domain in pool.map(fault,plans):checked[domain]+=1
  if resource:
   def bound(row):
    total=0
    for axis,key in enumerate(['allocated','peak','work']):
     for delta in [-1,0,1]:
      value,wire=invoke(row['mode'],'--limits',axis,row[key]+delta,int(delta<0))
      assert value['failed']==(delta<0)
      if delta<0:assert len(wire)<=(0 if not row['mode'] else 1)
      else:assert len(wire)==({0:1,1:3,8:2,9:2,10:3,11:2}[row['mode']])
      total+=1
    return total
   with ThreadPoolExecutor(max_workers=2) as pool:axes=sum(pool.map(bound,baselines))
  print(json.dumps(dict(cases=len(baselines),baselines=baselines,full_FI=resource,checked=checked,axes=axes,
   actual_Source_document_and_transport=True,complete_frames=True,all_domains_zero=True,image_sha256=image)))
 finally:
  assert all(p.read_bytes()==b for p,b in original.items()) and hashlib.sha256(exe.read_bytes()).hexdigest()==image
  assert all(not (root/str(i)/n).exists() for i in range(12) for n in ['root.xr','peer.xr'])
