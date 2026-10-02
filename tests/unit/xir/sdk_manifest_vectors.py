"""Independent canonical framing and closed manifest attacks on the actual SDK."""
from pathlib import Path
import argparse, concurrent.futures, hashlib, json, shutil, struct, subprocess, tempfile


def identity(manifest):
    prefix=('schema wire semantic value_abi call_abi program_abi architecture object_format hosted '
            'c_dialect crt sanitizers allocator assertions build_provider abi_recipe_version '
            'closure_recipe_version').split()
    def u32(value):return struct.pack('<I',value)
    def text(value):
        value=value.encode('utf-8');return u32(len(value))+value
    chunks=[b'xray:xir-runtime-sdk:v1',struct.pack('<17I',*(manifest[name] for name in prefix))]
    chunks += [text(manifest[name]) for name in ('target_triple','abi_recipe','closure_recipe')]
    chunks += [u32(len(manifest['abi_measurements']))]
    chunks += [struct.pack('<II',row['id'],row['value']) for row in manifest['abi_measurements']]
    chunks += [u32(len(manifest['files']))]
    chunks += [text(row['path'])+u32(row['kind'])+struct.pack('<Q',row['length'])+
               bytes.fromhex(row['sha256']) for row in manifest['files']]
    chunks += [u32(len(manifest['system_libraries']))]
    chunks += [text(name) for name in manifest['system_libraries']]
    return hashlib.sha256(b''.join(chunks)).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',required=True);parser.add_argument('--bundle',type=Path,required=True)
    args=parser.parse_args();bundle=args.bundle.resolve();original=json.loads((bundle/'sdk_manifest.json').read_text())
    with tempfile.TemporaryDirectory(prefix='xir-sdk-gates-') as temporary:
        directory=Path(temporary);cases=[]
        def add(name,value,status=1):
            path=directory/(name+'.json')
            path.write_bytes(value if isinstance(value,bytes) else json.dumps(value,ensure_ascii=False).encode())
            cases.append((name,path,status))
        def modified():return json.loads(json.dumps(original))
        add('same',original,0);add('root-reordered',dict(reversed(list(original.items()))),0)
        escaped=json.dumps(original).replace('"schema"','"\\u0073chema"').replace('kernel32','kernel\\u0033\\u0032')
        add('escaped-same',escaped.encode(),0)
        for name in list(original)[:17]:
            value=modified();value[name]+=1;add('prefix-'+name,value,6 if name in ('sanitizers','allocator') else 1)
        for name,old in (('schema',1),('semantic',57),('value_abi',16),('call_abi',20),('program_abi',25)):
            value=modified();value[name]=old;add('old-'+name,value)
        for i,row in enumerate(original['abi_measurements']):
            value=modified();value['abi_measurements'][i]['value']=row['value']+1;add('abi-'+str(row['id']),value)
        for name in original:
            value=modified();del value[name];add('missing-'+name,value)
        for name,token in (('negative',b'-1'),('fraction',b'2.5'),('bool',b'true'),('leading',b'02'),
                           ('overflow',b'18446744073709551616')):
            raw=json.dumps(original).encode().replace(b'"schema": 2',b'"schema": '+token,1)
            add(name,raw,3 if name=='overflow' else 1)
        for name,raw in (('duplicate',b'{"schema":2,'+json.dumps(original).encode()[1:]),
                         ('unknown',b'{"other":0,'+json.dumps(original).encode()[1:]),
                         ('tail',json.dumps(original).encode()+b'{}'),
                         ('utf8',json.dumps(original).encode().replace(b'kernel32',b'\xc0\x80')),
                         ('nul',json.dumps(original).encode().replace(b'kernel32',b'kernel\\u000032')),
                         ('surrogate',json.dumps(original).encode().replace(b'kernel32',b'\\ud800'))):add(name,raw)
        for name in ('path','kind','length','sha256'):
            value=modified();row=value['files'][0]
            row[name]={'path':'../lib/xray_xir_admission.lib','kind':1,'length':row['length']+1,
                       'sha256':'0'*64}[name];add('file-'+name,value)
        value=modified();value['files'][0],value['files'][1]=value['files'][1],value['files'][0];add('file-order',value)
        value=modified();value['files'].append(value['files'][0]);add('file-extra',value)
        value=modified();value['files'].pop();add('file-missing',value)
        value=modified();value['abi_measurements'][1]=value['abi_measurements'][0];add('abi-duplicate',value)
        def run(case,root=bundle):
            name,path,status=case
            result=subprocess.run([args.executable,str(root),str(path),'status'],capture_output=True,timeout=60)
            assert not result.returncode,(name,result.stderr.decode(errors='replace'))
            facts=result.stdout.decode().split();assert int(facts[0])==status,(name,facts,status)
            if status==0:assert int(facts[1])==len(original['files']) and facts[2]==identity(original)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:list(executor.map(run,cases))
        first=original['files'][0]['path'];shadow=directory/'bundle'
        for row in original['files']:
            target=shadow/row['path'];target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(bundle/row['path'],target)
        good=cases[0][1];run(('copied-root',good,0),shadow)
        target=shadow/first;saved=target.read_bytes();target.write_bytes(saved[:-1]+bytes([saved[-1]^1]))
        run(('changed-file',good,1),shadow);target.write_bytes(saved)
        absent=target.with_suffix('.absent');target.rename(absent);run(('absent-file',good,2),shadow)
        target.mkdir();run(('directory-file',good,1),shadow);target.rmdir();absent.rename(target)
        src=shadow/'src';moved=shadow/'original-source';src.rename(moved)
        junction=subprocess.run(['cmd','/d','/c','mklink','/J',str(src),str(moved)],capture_output=True)
        assert not junction.returncode,junction.stderr.decode(errors='replace')
        run(('reparse-ancestor',good,1),shadow)
        src.rmdir();moved.rename(src)
    print('SDK independent identity and '+str(len(cases))+' closed manifest/ABI attacks; actual file/ancestor gates PASS')


if __name__=='__main__':main()
