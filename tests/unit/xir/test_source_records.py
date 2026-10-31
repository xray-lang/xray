"""Exercise immutable original README fences and exact record rules through the current CLI."""
from pathlib import Path
import argparse, json, os, re, sys, tempfile

EXPECTED = b"1\n2\n3\nhello, alice\n"
POSITIVE = {
    "nullable_map_field": ('var scores=#{"alice":95}\nvar user={name:"alice",score:scores["alice"]}\nprint(user.score!)\n', b"95\n"),
    "shape_order": ('''fn name(v:{name:string,score:i64})->string {return v.name}
var first={score:5,name:"alice"}
var second:{score:i64,name:string}={name:"bob",score:7}
first=second
print(name(first))
print(first.score)
print(typeName(first))
''', b"bob\n7\nobject\n"),
    "eval_order": ('''var order=0
fn tick(n:i64)->i64 {order=order*10+n;return n}
var value={z:tick(1),a:tick(2)}
print(order)
print(value.a)
print(value.z)
''', b"12\n2\n1\n"),
    "value_copy": ('''var first={x:1,label:"old"}
var second=first
first.x=42
first.label="new"
print(first.x)
print(second.x)
print(second.label)
''', b"42\n1\nold\n"),
}
NEGATIVE = {
    "duplicate": ('var value={x:1,x:2}\n', b"duplicate record field"),
    "missing": ('fn f(v:{x:i64,y:i64})->i64 {return v.x}\nprint(f({x:1}))\n', b"exact contextual shape"),
    "extra": ('fn f(v:{x:i64})->i64 {return v.x}\nprint(f({x:1,y:2}))\n', b"outside its exact shape"),
    "field_type": ('var value:{x:i64}={x:"wrong"}\n', b"expression cannot satisfy its declared type"),
    "different_shape": ('var a={x:1}\nvar b={y:2}\na=b\n', b"expression cannot satisfy its declared type"),
    "unknown_field": ('var value={x:1}\nprint(value.y)\n', b"field"),
    "const_write": ('const value={x:1}\nvalue.x=2\n', b"mutable"),
    "nominal_distinct": ('struct Named {x:i64}\nvar value:Named={x:1}\n', b"expression cannot satisfy its declared type"),
}

def main():
    p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--native',action='store_true');a=p.parse_args()
    sys.path.insert(0,str(a.root/'scripts'));from xray_process import run_child
    out=Path(tempfile.mkdtemp(prefix='xray-records-'));commands=[]
    def run(name,argv):
        stdout=out/(name+'.stdout');stderr=out/(name+'.stderr')
        with stdout.open('xb') as o,stderr.open('xb') as e:
            code=run_child([str(x) for x in argv],cwd=out,env=os.environ.copy(),stdout=o,stderr=e,timeout=30)
        commands.append(dict(name=name,args=[str(x) for x in argv],exit=code));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        return code,stdout.read_bytes(),stderr.read_bytes()
    cases=[]
    for n in ['README.md','README_CN.md']:
        blocks=re.findall(r'^```xray[^\r\n]*\r?\n(.*?)^```', (a.root/n).read_text(encoding='utf-8'),re.M|re.S)
        assert len(blocks)==3
        assert blocks[1].encode()==(a.root/"tests/fixtures/xir_records/readme.xr").read_bytes()
        source=out/(n+'.xr');source.write_bytes(blocks[1].encode());cases.append((n,source,EXPECTED))
    for n,(text,expected) in POSITIVE.items():
        source=out/(n+'.xr');source.write_bytes(text.encode());cases.append((n,source,expected))
    for n,source,expected in cases:
        r=run(n+'-check',[a.binary,'check',source]);assert r==(0,b'',b''),(n,r,out)
        r=run(n+'-vm',[a.binary,'run',source]);assert r==(0,expected,b''),(n,r,out)
        generated=out/(n+'.c');r=run(n+'-emit',[a.binary,'build','-c',source,'-o',generated]);assert r[0]==0 and not r[2] and generated.is_file(),(n,r,out)
        if a.native:
            executable=out/(n+'.exe');r=run(n+'-build',[a.binary,'build',source,'-o',executable]);assert r[0]==0 and not r[2] and executable.is_file(),(n,r,out)
            r=run(n+'-native',[executable]);assert r==(0,expected,b''),(n,r,out)
    for n,(text,message) in NEGATIVE.items():
        source=out/(n+'.xr');source.write_bytes(text.encode());r=run(n,[a.binary,'check',source]);assert r[0]==1 and not r[1] and message in r[2],(n,r,out)
    print('two original README fences, four exact shape/order/value positives, eight fail-closed negatives'+('/native' if a.native else '')+' PASS '+str(out))

if __name__=='__main__':main()
