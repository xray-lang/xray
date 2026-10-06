"""Regenerate the full seventeen Source programs through the public producer."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
NAMES=('integer','string','generic','context_ordinary','context_go','context_existing','context_spawn',
       'grouped','import_default','const_state','local_storage','unknown_task','value_error','shadow_class',
       'context_match','context_match_block','context_match_existing')
p=argparse.ArgumentParser();p.add_argument('--producer',required=True);p.add_argument('--output',required=True)
a=p.parse_args();out=Path(a.output);out.mkdir(parents=True,exist_ok=True);rows=[];entries=[]
for name in NAMES:
 target=out/('source17_'+name+'.c')
 result=subprocess.run([a.producer,str(target),name],capture_output=True,timeout=300)
 (out/(name+'.stdout')).write_bytes(result.stdout);(out/(name+'.stderr')).write_bytes(result.stderr)
 if result.returncode:raise SystemExit('genuine Source producer failed '+name+'\n'+result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace'))
 entry=re.search(rb'^'+name.encode()+rb' entry=(\d+) bytes=(\d+)\r?$',result.stdout,re.M)
 if not entry:raise SystemExit('missing public producer entry receipt '+name)
 raw=target.read_bytes()
 if len(raw)!=int(entry[2]) or b'({' in raw or b'XR_XIR_CALL_ABI_VERSION == 28u' not in raw:raise SystemExit('invalid genuine C receipt '+name)
 entries.append(int(entry[1]));rows.append({'name':name,'entry':entries[-1],'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'producer_argv':[a.producer,str(target),name]})
header=''.join('XR_DATA const XrXirProgramSpec source17_'+name+'_program;\n' for name in NAMES)
header+='static const XrXirProgramSpec *const source17_specs[] = {'+','.join('&source17_'+name+'_program' for name in NAMES)+'};\n'
header+='static const uint32_t source17_entries[] = {'+','.join(map(str,entries))+'};\n'
(out/'source17_generated.h').write_text(header,encoding='utf-8',newline='\n')
(out/'generation.json').write_text(json.dumps({'status':'GENUINE_SOURCE17_REGENERATED','cases':rows},indent=2)+'\n',encoding='utf-8')
