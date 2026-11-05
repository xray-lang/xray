"""Execute the complete variables demo and exact, atomic Array binding rules."""
from pathlib import Path
import argparse, hashlib, json, os, sys, tempfile

MESSAGE = b"Array binding requires its exact element count"
POSITIVE = {
    "equal": ('var [a,b,c]=[10,20,30]\nconst [d]=[42]\nprint(a+b+c)\nprint(d)\n', b"60\n42\n"),
    "skip_once_owned": ('var calls=0\nfn values()->Array<string>{calls+=1;return ["first","skip","last"]}\nconst [a,,c]=values()\nprint(calls)\nprint(a)\nprint(c)\n', b"1\nfirst\nlast\n"),
    "empty": ('var calls=0\nfn empty()->Array<i64>{calls+=1;return []}\nconst []=empty()\nprint(calls)\n', b"1\n"),
    "owned_return": ('fn first()->string{const [a,b]=["retired","discard"];return a}\nprint(first())\n', b"retired\n"),
    "object": ('var person={first:"Bob",score:99}\nconst {first:label,score}=person\nperson.first="other"\nprint(label)\nprint(score)\n', b"Bob\n99\n"),
    "short_long": ('fn values(long:bool)->Array<i64>{if(long){return [1,2,3,4,5]};return [1]}\nvar outer=99\ntry {var [outer,b]=values(false);print("bad-short")}catch panic(p){print(p.code);print(p.message)}\ntry {const [a,b]=values(true);print("bad-long")}catch panic(p){print(p.code);print(p.message)}\nprint(outer)\n', b"445\n"+MESSAGE+b"\n445\n"+MESSAGE+b"\n99\n"),
}
NEGATIVE = {
    "not_array": ('var [a]=42\n', b"Array binding requires an exact Array type"),
    "duplicate": ('var [a,a]=[1,2]\n', b"duplicate product binding name"),
    "const_write": ('const [a]=[1]\na=2\n', b"mutable"),
    "object_missing": ('var {missing}={x:1}\n', b"field"),
    "object_duplicate": ('var {x,x}={x:1}\n', b"duplicate object binding field"),
    "not_object": ('var {x}=1\n', b"object binding requires statically known fields"),
}

def main():
    p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--native',action='store_true');a=p.parse_args()
    sys.path.insert(0,str(a.root/'scripts'));from xray_process import run_child
    output=Path(tempfile.mkdtemp(prefix='xray-array-bindings-'));records=[]
    def run(name,command):
        stdout,stderr=output/(name+'.stdout'),output/(name+'.stderr')
        with stdout.open('xb') as o,stderr.open('xb') as e:
            code=run_child([str(x) for x in command],cwd=output,env=os.environ.copy(),stdout=o,stderr=e,timeout=30)
        records.append(dict(name=name,args=[str(x) for x in command],exit=code));(output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
        return code,stdout.read_bytes(),stderr.read_bytes()
    demo=a.root/'demos/01-basics/variables.xr';raw=demo.read_bytes()
    assert hashlib.sha256(raw.replace(b'\r\n',b'\n')).hexdigest()=='cddfa7a305717cd259ada93fbd6d1afbead9e991390a73c2762d68eec8dfa7bc'
    golden=(a.root/'tests/smoke/expected/variables.txt').read_bytes()
    assert len(golden)==153 and hashlib.sha256(golden).hexdigest()=='478beb16dba09a73ca05790a0716f6a3ae3b341008d24cf1f7ab5ad66d950e03'
    cases=[('variables',demo,golden)]
    for name,(text,expected) in POSITIVE.items():
        source=output/(name+'.xr');source.write_bytes(text.encode());cases.append((name,source,expected))
    for name,source,expected in cases:
        r=run(name+'-check',[a.binary,'check',source]);assert r==(0,b'',b''),(name,r,output)
        r=run(name+'-vm',[a.binary,'run',source]);assert r==(0,expected,b''),(name,r,output)
        if name=='variables':
            generated=output/'variables.c';r=run('emit',[a.binary,'build','-c',source,'-o',generated]);assert r[0]==0 and not r[2] and generated.is_file(),(r,output)
        if a.native:
            exe=output/(name+('.exe' if os.name=='nt' else ''));r=run(name+'-build',[a.binary,'build',source,'-o',exe]);assert r[0]==0 and not r[2] and exe.is_file(),(name,r,output)
            r=run(name+'-native',[exe]);assert r==(0,expected,b''),(name,r,output)
    for name,(text,message) in NEGATIVE.items():
        source=output/(name+'.xr');source.write_bytes(text.encode());r=run(name,[a.binary,'check',source]);assert r[0]==1 and not r[1] and message in r[2],(name,r,output)
    print('complete variables; exact/short/long/empty/skip/once/owned/object; six static refusals'+('/native' if a.native else '')+' PASS '+str(output))

if __name__=='__main__':main()
