from pathlib import Path
import hashlib,json,re,subprocess,sys
exe=Path(sys.argv[1]).resolve();mode=sys.argv[2];assert mode in ('--normal','--resources')
maximum=(1<<64)-1
valid=b'{"jsonrpc":"2.0","method":"probe/notice"}'
malformed=b'{"jsonrpc":"2.0","id":7,"method":}'
def frame(body):return b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body
def unpack(data):
    values=[]
    while data:
        header,sep,rest=data.partition(b'\r\n\r\n');assert sep and re.fullmatch(rb'Content-Length: [0-9]+',header)
        length=int(header.split(b': ')[1]);assert 0<length<=len(rest);values.append(json.loads(rest[:length]));data=rest[length:]
    return values
def invoke(fault,scenario):
    body=malformed if scenario==0 else valid if scenario==1 else b'"\xff"' if scenario==3 else b'"\n"'
    payload=frame(valid)+frame(body)+frame(valid) if scenario==0 else frame(body)+frame(valid)
    r=subprocess.run([str(exe),str(fault),str(scenario)],input=payload,capture_output=True,timeout=25)
    assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
    assert b'AddressSanitizer' not in r.stderr and b'runtime error:' not in r.stderr
    m=re.search(rb'FRAME scenario=(\d+) frames=(\d+) dispatches=(\d+) sites=(\d+) injected=(\d+) exit=(\d+) JSON_and_transport0/0 dispatch_spy=1',r.stderr)
    assert m,r.stderr
    stats=list(map(int,m.groups()));values=unpack(r.stdout)
    for value in values:assert value=={'jsonrpc':'2.0','id':None,'error':{'code':-32700,'message':'Parse error'}},value
    if fault==maximum:
        assert stats[1:3]==([3,2] if scenario==0 else [2,2] if scenario==1 else [2,1]) and stats[4:]==[0,0],stats
        assert len(values)==(0 if scenario==1 else 1)
    else:
        assert stats[4:]==[1,1],stats
        assert stats[1:3]==([2,1] if scenario==0 else [1,0]) and not values,(stats,values)
    print(json.dumps(dict(scenario=scenario,fault=fault,stats=stats,stderr=r.stderr.decode('utf-8'),wire=values)))
    return stats[3]
measured={scenario:invoke(maximum,scenario) for scenario in (0,1,3,4)}
for scenario,sites in measured.items():
    for fault in range(sites if mode=='--resources' else 1):invoke(fault,scenario)
print(json.dumps(dict(mode=mode,measured=measured,full_FI=mode=='--resources',image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),dispatch_spy=True,public_server=False)))
