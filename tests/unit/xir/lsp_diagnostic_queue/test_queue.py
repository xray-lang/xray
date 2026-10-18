from pathlib import Path
import hashlib,json,os,subprocess,sys,tempfile
image=Path(sys.argv[1]).resolve();stdlib=Path(sys.argv[2]).resolve();mode=sys.argv[3]
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib)}
with tempfile.TemporaryDirectory(prefix='xray-diagnostic-queue-文档-',dir=os.getcwd()) as folder:
 root=Path(folder);(root/'xray.toml').write_text('[project]\nname="queue"\nversion="1.0.0"\nmain="root.xr"\n',encoding='utf-8')
 (root/'root.xr').write_text('export fn main()->i64 { return 1 }\n',encoding='utf-8')
 child=subprocess.run([str(image),(root/'root.xr').as_uri(),mode],env=env,capture_output=True,timeout=20)
 if mode.startswith('--before-'):
  text='deadline_wrapped_to_zero' if mode=='--before-deadline' else 'growth_OOM_lost_work'
  expected=f'QUEUE_BEFORE {text}=1 compiler0/0 JSON0/0 app0/0\n'.encode()
  assert child.returncode==1 and not child.stdout and child.stderr.replace(b'\r\n',b'\n')==expected,(child.returncode,child.stdout,child.stderr)
  print('Exact predecessor counterexample observed: '+expected.decode().strip())
 else:
  assert mode in ['--normal','--resources']
  expected=f'Diagnostic queue sites=4 full_FI={int(mode=="--resources")} invalid=10 deduplicated=65 actual_changed_Source_OOM_exit=1 owned_cleanup=1 compiler0/0 JSON0/0 app0/0\n'.encode()
  assert child.returncode==0 and not child.stderr and child.stdout.replace(b'\r\n',b'\n')==expected,(child.returncode,child.stdout,child.stderr)
  print(expected.decode().strip())
 print(json.dumps(dict(mode=mode,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),actual_document_owner=True,controlled_clock=True,public_LSP_process=False)))
