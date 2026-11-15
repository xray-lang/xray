"""Retained nominal carrier duties through the current real CLI.

Source export declarations remain intact. This gate makes no claim about the
retired hosted-fragment command or its xr_probe C ABI. Supported products use
independent probe calls and exact values in both VM and native execution.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/lib'))
from xraytest import proc

cases = {
    "nullable_local": 'struct Row { name: string }\nexport fn probe() -> string { var row: Row? = Row{ name: "a" }; return row!.name }',
    "nullable_module": 'struct Row { name: string }\nvar row: Row? = null\nexport fn probe() -> string { row = Row{ name: "a" }; return row!.name }',
    "index": '''struct Row { name: string }
fn read(xs: Array<Row>, index: i64) -> string { return xs[index].name }
export fn probe(index: i64) -> string {
var xs: Array<Row> = [Row{ name: "a" }]
return read(xs, index)
}''',
    "merge": '''struct Row { name: string }
fn read(flag: bool, a: Row, b: Row) -> string { var row = flag ? a : b; return row.name }
export fn probe(flag: bool) -> string {
var a: Row = Row{ name: "a" }
var b: Row = Row{ name: "b" }
return read(flag, a, b)
}''',
    "nested_index": '''struct Child { name: string }
struct Row { child: Child }
fn read(xs: Array<Row>, index: i64) -> string { return xs[index].child.name }
export fn probe(index: i64) -> string {
var xs: Array<Row> = [Row{ child: Child{ name: "a" } }]
return read(xs, index)
}''',
}
cases["nullable_call_merge"] = 'struct Row { name: string }\nfn make() -> Row? { return Row{ name: "a" } }\nexport fn probe(flag: bool) -> string { var row: Row? = flag ? make() : null; if (row == null) { return "empty" }; return row!.name }\n'
for scalar in ("u64", "i64", "f64", "bool"):
    cases["scalar_call_" + scalar] = (
        "struct Row { value: " + scalar + " }\n"
        "fn take(value: " + scalar + ") -> " + scalar + " { return value }\n"
        "export fn probe(value: " + scalar + ") -> " + scalar +
        " { var row: Row = Row{ value: value }; return take(row.value) }\n"
    )

CALLS={
    'nullable_local':('print(probe())\n','a\n'),
    'nullable_module':('print(probe())\n','a\n'),
    'index':('print(probe(0))\n','a\n'),
    'merge':('print(probe(true))\nprint(probe(false))\n','a\nb\n'),
    'nested_index':('print(probe(0))\n','a\n'),
    'nullable_call_merge':('print(probe(true))\nprint(probe(false))\n','a\nempty\n'),
    'scalar_call_u64':('print(probe(7))\n','7\n'),
    'scalar_call_i64':('print(probe(-7))\n','-7\n'),
    'scalar_call_f64':('print(probe(2.5))\n','2.5\n'),
    'scalar_call_bool':('print(probe(true))\nprint(probe(false))\n','true\nfalse\n'),
}
UNACCEPTED={
    'index':'Array<Row> physical ARRAY_NEW carrier',
    'nested_index':'Array<Row> with nested nominal Child physical carrier',
    'scalar_call_u64':'u64 nominal record field physical carrier',
}


def command(arguments,cwd,expected=None):
    result=proc.run(arguments,cwd=cwd,timeout=30,env={**os.environ,'NO_COLOR':'1'})
    assert result.ok,(arguments,result.returncode,result.timed_out,result.stdout,result.stderr)
    if expected is not None:
        assert result.stdout.replace(b'\r\n',b'\n')==expected.encode('utf-8'),(arguments,result.stdout,expected)
        assert not result.stderr,(arguments,result.stderr)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,required=True)
    parser.add_argument('--host-compiler',help='Actual native compiler selected through current CLI -C')
    parser.add_argument('--source-only',action='store_true')
    parser.add_argument('--include-unaccepted',action='store_true')
    parser.add_argument('--case',choices=tuple(cases))
    args=parser.parse_args();binary=str(args.compiler.resolve(strict=True))
    selected={args.case:cases[args.case]} if args.case else cases
    assert len(cases)==10 and set(cases)==set(CALLS)
    with tempfile.TemporaryDirectory(prefix='xray-nominal-carrier-') as temporary:
        folder=Path(temporary)
        for name,source in selected.items():
            assert 'export fn probe' in source and 'type Row = {' not in source
            path=folder/(name+'.xr');path.write_text(source+'\n'+CALLS[name][0],encoding='utf-8')
            command([binary,'check',str(path)],ROOT)
        products=0
        if not args.source_only:
            for name in selected:
                if name in UNACCEPTED and not args.include_unaccepted:continue
                path=folder/(name+'.xr');expected=CALLS[name][1]
                command([binary,'run',str(path)],ROOT,expected)
                generated=folder/(name+'.c')
                command([binary,'build','-c','-o',str(generated),str(path)],ROOT)
                assert generated.is_file() and generated.stat().st_size>0
                native=folder/(name+('.exe' if os.name=='nt' else ''))
                compiler=['-C',args.host_compiler] if args.host_compiler else []
                command([binary,'build',*compiler,'-o',str(native),str(path)],ROOT)
                command([str(native)],ROOT,expected);products+=1
                print(name+': exact VM/native probe values and verified C PASS',flush=True)
    remaining=[name for name in selected if name in UNACCEPTED]
    print(f'{len(selected)} nominal Source export checks; {products} actual VM/native products; '
          f'{len(remaining)} retained physical dependencies remain unaccepted')
    for name in remaining:print(name+': UNACCEPTED '+UNACCEPTED[name])
    print('Retired hosted-fragment/xr_probe C ABI is not qualified by this gate')
    return 0


if __name__=='__main__':raise SystemExit(main())
