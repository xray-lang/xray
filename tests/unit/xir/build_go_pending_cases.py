"""Regenerate six frozen Built graphs through the canonical public C emitter."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser()
p.add_argument('--task-producer',required=True)
p.add_argument('--variant-producer',required=True)
p.add_argument('--output',required=True)
a=p.parse_args();out=Path(a.output);out.mkdir(parents=True,exist_ok=True);rows=[]
for family,producer,names in (('pending',a.task_producer,('body','call','await')),
                              ('pending_variant',a.variant_producer,('body','nested','write'))):
 for name in names:
  target=out/(family+'_'+name+'.c');argv=[producer,str(target),name]
  result=subprocess.run(argv,capture_output=True,timeout=180)
  (out/(family+'_'+name+'.stdout')).write_bytes(result.stdout)
  (out/(family+'_'+name+'.stderr')).write_bytes(result.stderr)
  if result.returncode:raise SystemExit('genuine producer failed '+family+'/'+name+'\n'+result.stderr.decode(errors='replace'))
  raw=target.read_bytes()
  if b'XR_XIR_CALL_ABI_VERSION == 28u' not in raw or b'({' in raw:raise SystemExit('invalid generated C identity or C11')
  rows.append({'family':family,'name':name,'argv':argv,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()})
(out/'generation.json').write_text(json.dumps({'status':'GENUINE_SIX_GRAPH_REGENERATION','cases':rows},indent=2)+'\n')
