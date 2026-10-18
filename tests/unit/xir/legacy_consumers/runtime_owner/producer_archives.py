"""Bind the actual public fault consumer's archive closure and content identities."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args = parser.parse_args()
build = args.build.resolve(strict=True)
command = subprocess.check_output(['ninja','-C',str(build),'-t','commands','test_source_public_faults'],text=True,encoding='utf-8').splitlines()[-1]
tail = command.split('/subsystem:console',1)[1]
libraries = re.findall(r'(?<![:\w])([\w\\./-]+\.lib)\b',tail)
archives = [build/path.replace('\\','/') for path in libraries if (build/path.replace('\\','/')).is_file()]
if not archives or not any(p.name=='xray_source_api.lib' for p in archives):
    raise ValueError('actual public API production archive closure is absent')
args.output.write_text('set(XR_PRODUCER_ARCHIVES\n'+''.join('    "'+p.as_posix()+'"\n' for p in archives)+')\n',encoding='utf-8')
args.output.with_suffix('.json').write_text(json.dumps({'source_command':command,'archives':[
    {'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in archives]},indent=2)+'\n',encoding='utf-8')
