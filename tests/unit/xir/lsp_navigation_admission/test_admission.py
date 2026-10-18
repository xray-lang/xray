from pathlib import Path
import hashlib,json,os,re,subprocess,sys,tempfile
exe,stdlib=map(Path,sys.argv[1:3]);mode=sys.argv[3]
assert mode in ('--normal','--resources','--counterfactual')
env={**os.environ,'XRAY_STDLIB_PATH':str(stdlib.resolve())}
def unpack(data):
    values=[]
    while data:
        head,separator,rest=data.partition(b'\r\n\r\n');assert separator and re.fullmatch(rb'Content-Length: [0-9]+',head)
        size=int(head.split(b': ')[1]);assert 0<size<=len(rest);values.append(json.loads(rest[:size]));data=rest[size:]
    return values
rows=[]
with tempfile.TemporaryDirectory(prefix='navigation-admission-😀-#-',dir=os.getcwd()) as folder:
    root=Path(folder);(root/'xray.toml').write_text('[project]\nname="navigation-admission"\nmain="root.xr"\n',encoding='utf-8')
    uri,library=[(root/f'{name}.xr').as_uri() for name in ('root','lib')]
    for method in range(4):
        cases=[*range(9)] if method==1 else [0,1,2,3,4,7,8]
        for case in cases if mode=='--counterfactual' else [None]:
            kind='--before' if case is not None else '--full' if mode=='--resources' else '--normal'
            result=subprocess.run([str(exe.resolve()),uri,'--nav-admission',library,str(method),kind,*([str(case)] if case is not None else [])],
                input=b'',stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,timeout=20)
            assert result.returncode==(1 if case is not None else 0),(method,case,result.returncode,result.stderr[-3500:])
            match=re.search(rb'NAV_ADMISSION mode=(\d+) checks=(\d+) case=(\d+) accepted=(\d+) physical0/0 JSON0/0 app0/0',result.stderr)
            assert match and int(match[1])==method and int(match[4])==(1 if case is not None else 0),result.stderr[-3500:]
            messages=unpack(result.stdout);assert len(messages)==1 and messages[0]['id']=='source\0λ',messages
            value=messages[0]
            if case is not None:
                assert int(match[3])==case and value['error']['code']==-32800,value
            else:
                assert int(match[2])==(14 if method==1 else 11)
                if method==0:assert value['result']['uri']==library,value
                elif method<3:assert len(value['result'])==1,value
                else:assert 'value' in value['result']['contents']['value'],value
                probe=re.search(rb'NAV_CAPTURE mode=(\d+) source=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) full=(\d+) axes=(\d+) physical0/0',result.stderr)
                assert probe and int(probe[1])==method and all(int(probe[n])>0 for n in (2,3,4,5))
                assert int(probe[6])==(1 if mode=='--resources' else 0) and int(probe[7])==(9 if mode=='--resources' else 0)
            rows.append(dict(method=method,case=case,checks=int(match[2]),source_sites=int(probe[2]) if case is None else None))
print(json.dumps(dict(image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),mode=mode,cases=len(rows),rows=rows),ensure_ascii=False))
