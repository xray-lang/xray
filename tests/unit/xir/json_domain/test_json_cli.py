"""Exact folded JSON static domain and authentic prelude shadowing.

No parse/codec API or physical JSON execution is qualified by these checks.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def check(binary: Path, path: Path, source: str, success: bool) -> bytes:
    path.write_text(source,encoding="utf-8")
    result=subprocess.run([str(binary),"check",str(path)],capture_output=True,timeout=30,
                          env={**os.environ,"NO_COLOR":"1"})
    assert result.returncode==(0 if success else 1),(source,result.returncode,result.stdout,result.stderr)
    if not success:assert b"error:" in result.stderr,(source,result.stderr)
    return result.stderr


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True)
    args=parser.parse_args();binary=args.binary.resolve(strict=True)
    positives=[
        'struct User{name:string;age:i64}\nvar user=User{name:"Alice",age:30}\n'
        'var data:JSON.Object={"name":"Alice","age":30}\ndata["traceId"]="req-1"\n',
        'var object:JSON.Object={}\nvar map:Map<string,JSON.Value>=object\nvar same:JSON.Object=map\n',
        'var value:JSON.Value=null\nvar absent:JSON.Value?=null\nvar some:JSON.Value?=value\n',
        'var values:Array<JSON.Value>=[null,true,1,1.5,"text"]\n',
        'var path:JSON.Path=["user",0,"name"]\nvar same:Array<string|i64>=path\n',
        'class Map{}\nclass Array{}\nvar object:JSON.Object={"x":1}\nvar path:JSON.Path=["x",0]\n',
        'class JSON{}\nvar user:JSON=JSON()\n',
        'fn f(){var value:JSON.Value=null;type JSON=i64}\n',
        'fn unrelated<T>(){type JSON=i64}\nvar object:JSON.Object={"x":1}\n',
        'type Saved=JSON.Value\nfn f(){type JSON=i64;var value:Saved=null}\n',
    ]
    negatives=[
        'var value:JSON=null\n',
        'var value:JSON.Value=()\n',
        'var n:i8=1\nvar value:JSON.Value=n\n',
        'var n:f32=1.5\nvar value:JSON.Value=n\n',
        'var values:Array<JSON.Value>=[1]\nvar value:JSON.Value=values\n',
        'var object:JSON.Object={}\nvar value:JSON.Value=object\n',
        'struct Item{x:i64}\nvar item=Item{x:1}\nvar value:JSON.Value=item\n',
        'class JSON{}\nvar value:JSON.Value=null\n',
        'const JSON=1\nvar value:JSON.Value=null\n',
        'type JSON=i64\nvar value:JSON.Value=null\n',
        'fn f<JSON>(value:JSON){var bad:JSON.Value=null}\n',
        'var value:JSON.Value=1\nvar n=value.x\n',
        'var value:JSON.Value=1\nvar n=value[0]\n',
        'var value:JSON.Value=1\nvar n=len(value)\n',
        'var path:JSON.Path=[true]\n',
        'var values={"name":"Alice","age":30}\n',
    ]
    with tempfile.TemporaryDirectory(prefix="xray-json-static-") as temporary:
        folder=Path(temporary)
        for i,source in enumerate(positives):check(binary,folder/f"positive-{i}.xr",source,True)
        for i,source in enumerate(negatives):check(binary,folder/f"negative-{i}.xr",source,False)
        alias_refusals=[
            'fn f(){type JSON=i64;var value:JSON.Value=null}\n',
            'fn f(){type JSON=i64;var value:JSON.Value?=null}\n',
            'fn f(){type JSON=i64;var object:JSON.Object={"x":1}}\n',
            'fn f(){type JSON=i64;var path:JSON.Path=["x",0]}\n',
            'fn f<T>(){type JSON=i64;var object:JSON.Object={}}\n',
            'fn f(){{type JSON=i64};var value:JSON.Value=null}\n',
            'type JSON=i64\nvar value:JSON.Value=null\n',
            'type JSON=i64\nvar object:JSON.Object={}\n',
            'type JSON=i64\nvar path:JSON.Path=[]\n',
            'fn f<JSON>(){var value:JSON.Value=null}\n',
            'type Other=i64\nvar value:Other.Value=null\n',
        ]
        for i,source in enumerate(alias_refusals):
            diagnostic=check(binary,folder/f"alias-root-{i}.xr",source,False)
            assert b"type alias cannot be used as a namespace in a qualified type name" in diagnostic,diagnostic
        (folder/"library.xr").write_text('export struct Value{name:string}\n',encoding="utf-8")
        diagnostic=check(binary,folder/"import.xr",'import "./library" as JSON\nvar value:JSON.Value=null\n',False)
        assert b"null requires an exact nullable context" in diagnostic,diagnostic
    print(f"Folded JSON: {len(positives)} static positives and {len(negatives)+len(alias_refusals)+1} refusals PASS; codec/runtime unaccepted")
    return 0


if __name__=="__main__":raise SystemExit(main())
