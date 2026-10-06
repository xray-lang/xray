from pathlib import Path
import argparse,datetime,json,subprocess,sys,uuid
p=argparse.ArgumentParser();p.add_argument('--kind',choices=['baseline','matrix'],required=True);p.add_argument('--exe',type=Path,required=True);p.add_argument('--source-root',type=Path,required=True);p.add_argument('--build-root',type=Path,required=True);p.add_argument('--reports-root',type=Path,required=True);a=p.parse_args();here=Path(__file__).resolve().parent
out=a.reports_root/(datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+uuid.uuid4().hex[:12]);out.mkdir(parents=True)
commands=[]
def run(cmd):
 result=subprocess.run(cmd);commands.append({'argv':cmd,'exit':result.returncode});(out/'registration-commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert result.returncode==0
run([sys.executable,str(here/'bind_vm19_inputs.py'),'--source-root',str(a.source_root),'--build-root',str(a.build_root),'--exe',str(a.exe),'--out',str(out/'binding.json')])
common=['--exe',str(a.exe),'--binding',str(out/'binding.json')]
run([sys.executable,str(here/'run_vm19_fault_recipe.py'),'--mode','baseline',*common,'--out',str(out/'baseline')])
run([sys.executable,str(here/'run_vm19_fault_recipe.py'),'--mode','plan','--baseline',str(out/'baseline/baselines.json'),'--out',str(out/'plan')])
if a.kind=='matrix':
 run([sys.executable,str(here/'run_vm19_fault_recipe.py'),'--mode','run',*common,'--baseline',str(out/'baseline/baselines.json'),'--out',str(out/'matrix'),'--window-approved'])
