"""Exercise the real owned local locator with ready and cold environments."""
from pathlib import Path
import argparse,json,os,subprocess,tempfile,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[4]/"scripts"))
from xray_process import run_child
p=argparse.ArgumentParser();p.add_argument('--exe',type=Path,required=True);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--compiler',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True);out=Path(tempfile.mkdtemp(prefix='run-',dir=a.out));records=[]
def run(label,env,compiler,mode,status,activation):
 argv=[str(a.exe),'--open',str(a.sdk),str(compiler),mode,str(status),'1',str(activation)]
 with (out/(label+'.stdout')).open('xb') as stdout,(out/(label+'.stderr')).open('xb') as stderr:
  code=run_child(argv,env=env,stdout=stdout,stderr=stderr,timeout=120)
 r=subprocess.CompletedProcess(argv,code,(out/(label+'.stdout')).read_bytes(),(out/(label+'.stderr')).read_bytes())
 records.append(dict(label=label,argv=argv,exit=r.returncode,stdout=str(out/(label+'.stdout')),stderr=str(out/(label+'.stderr'))))
 (out/'result.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
 if r.returncode:
  print(r.stdout.decode('utf-8',errors='replace'));print(r.stderr.decode('utf-8',errors='replace'));raise SystemExit(r.returncode)
 print(label+' PASS',flush=True)
ready=dict(os.environ);run('ready',ready,a.compiler,'normal',0,0)
missing=dict(ready)
for key in list(missing):
 if key.upper()=='VCTOOLSINSTALLDIR':missing.pop(key)
run('missing-field-cold',missing,a.compiler,'normal',0,1)
arch=dict(ready);arch['VSCMD_ARG_TGT_ARCH']='x86';run('arch-mismatch-cold',arch,a.compiler,'normal',0,1)
other=out/'other-installation';other.mkdir();installation=dict(ready);installation['VSINSTALLDIR']=str(other)
# The real activation reports its inherited installation/toolset mismatch.
run('installation-mismatch-cold-child-failed',installation,a.compiler,'normal',8,1)
run('cold-explicit-wrong-compiler',arch,a.compiler.parent/'link.exe','normal',1,1)
run('explicit-wrong-compiler',ready,a.compiler.parent/'link.exe','normal',1,0)
run('eligibility-real-io',ready,a.compiler,'io',6,0)
run('eligibility-partial-copy-oom',ready,a.compiler,'copy',5,0)
invalid=dict(ready);invalid['WindowsSDKVersion']='not-a-version';run('eligible-invalid-sdk-no-cold',invalid,a.compiler,'normal',1,0)
poison=dict(ready);poison.update(CL='/invalid-cl-marker',_CL_='/invalid-cl-marker',LINK='/invalid-link-marker',_LINK_='/invalid-link-marker',INCLUDE='X:/not-an-include',LIB='X:/not-a-library')
run('markers-grant-no-authority',poison,a.compiler,'normal',0,0)
print('Actual prepared snapshot: ready/cold/error identity and same-C-process environment invariance PASS')
