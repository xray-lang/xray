"""Verify explicit ordinary methods and unchanged public storage fields."""
from pathlib import Path
import argparse, json, os, sys, tempfile

SOURCE = """class Meter {
    value:i64
    init(v:i64){this.value=v}
    read()->i64{return this.value}
}
class Parts {year:i64;id:i64;init(){this.year=2024;this.id=11}}
var m=Meter(7)
print(m.value)
print(m.read())
m.value=9
print(m.value)
print(m.read())
var parts=Parts()
print(parts.year)
print(parts.id)
"""
EXPECTED = b"7\n7\n9\n9\n2024\n11\n"

def main():
    p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--native',action='store_true');a=p.parse_args()
    sys.path.insert(0,str(a.root/'scripts'));from xray_process import run_child
    out=Path(tempfile.mkdtemp(prefix='xray-method-surface-'));records=[]
    def run(name,args):
        stdout,stderr=out/(name+'.stdout'),out/(name+'.stderr')
        with stdout.open('xb') as o,stderr.open('xb') as e:
            code=run_child([str(x) for x in args],cwd=out,env=os.environ.copy(),stdout=o,stderr=e,timeout=30)
        records.append(dict(name=name,args=[str(x) for x in args],exit=code));(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
        return code,stdout.read_bytes(),stderr.read_bytes()
    source=out/'methods.xr';source.write_bytes(SOURCE.encode())
    r=run('check',[a.binary,'check',source]);assert r==(0,b'',b''),(r,out)
    r=run('vm',[a.binary,'run',source]);assert r==(0,EXPECTED,b''),(r,out)
    generated=out/'methods.c';r=run('emit',[a.binary,'build','-c',source,'-o',generated]);assert r[0]==0 and not r[2] and generated.is_file(),(r,out)
    if a.native:
        exe=out/('methods.exe' if os.name=='nt' else 'methods');r=run('build',[a.binary,'build',source,'-o',exe]);assert r[0]==0 and not r[2] and exe.is_file(),(r,out)
        r=run('native',[exe]);assert r==(0,EXPECTED,b''),(r,out)
    source=out/'no-implicit-call.xr';source.write_bytes(b'class M {init(){} read()->i64{return 7}}\nvar m=M()\nvar result:i64=m.read\n')
    r=run('no-implicit-call',[a.binary,'check',source]);assert r[0]==1 and not r[1] and b':3:18: error: expression cannot satisfy its declared type' in r[2],(r,out)
    print('ordinary explicit methods/public fields/no implicit getter'+('/native' if a.native else '')+' PASS '+str(out))

if __name__=='__main__':main()
