"""Compare owned string recipes with independent UTF-8 repetition expectations."""
from __future__ import annotations
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path);args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve();binary.relative_to(root);temporary=None
    if args.output:
        output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:
        temporary=tempfile.TemporaryDirectory(prefix='string-owned-',dir=binary.parent);output=Path(temporary.name)
    records=[]
    try:
        stdlib=output/'stdlib';(stdlib/'types').mkdir(parents=True)
        poison=stdlib/'types/string.xr';poison.write_text('invalid string declaration source\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1');hidden=dict(env,PATH=str(output/'no-C-provider'))
        source='fn row(value:string,count:i64) { print(value.toString(),value.repeat(count)) }\n'
        expected=bytearray();pairs=[]
        for text in ['', 'ab', 'é😀', 'a\0b']:
            spelling=json.dumps(text,ensure_ascii=False).replace('\\u0000','\\0')
            for count in [-9223372036854775808,-1,0,1,2,3,4,7,8,15,16,31,32,63,64,65]:
                pairs.append((text,count));source+=f'row({spelling},{count})\n'
                expected.extend((text+' '+text*max(0,count)+'\n').encode('utf-8'))
        source+='print("".repeat(9223372036854775807))\n';expected.extend(b'\n')
        for text,count in [('ab',1024),('x',4095),('x',4096),('x',4097)]:
            source+=f'print("{text}".repeat({count}))\n'
            expected.extend((text*count+'\n').encode())
        source+='print(len("ab".repeat(1024)),len("x".repeat(4095)),len("x".repeat(4096)),len("x".repeat(4097)))\n'
        expected.extend(b'2048 4095 4096 4097\n')
        source+=('fn snapshot()->string {\n var value:string="ab"\n var hits:i64=0\n'
                 ' var change=fn()->i64 { value="X"; hits=hits+1; return 3 }\n'
                 ' var result=value.repeat(change())\n print(value,hits)\n return result\n}\nprint(snapshot())\n')
        expected.extend(b'X 1\nababab\n')
        case=output/'words.xr';case.write_text(source,encoding='utf-8')
        def invoke(name,executable,arguments,environment):
            result=subprocess.run([str(executable),*arguments],cwd=root,env=environment,capture_output=True,timeout=60)
            (output/(name+'.stdout')).write_bytes(result.stdout);(output/(name+'.stderr')).write_bytes(result.stderr)
            records.append(dict(name=name,argv=[str(executable),*arguments],exit_code=result.returncode,
                stdout_sha256=hashlib.sha256(result.stdout).hexdigest(),stderr_sha256=hashlib.sha256(result.stderr).hexdigest()))
            (output/'commands.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8');return result
        result=invoke('words-vm',binary,['run',str(case)],hidden)
        assert (result.returncode,result.stdout,result.stderr)==(0,bytes(expected),b''),result
        generated=output/'words.c';result=invoke('words-c',binary,['build','--c-only',str(case),'-o',str(generated)],env)
        assert result.returncode==0 and not result.stderr and b'({' not in generated.read_bytes(),result
        native=output/'words.exe';result=invoke('words-build',binary,['build',str(case),'-o',str(native)],env)
        assert result.returncode==0 and not result.stderr,result
        result=invoke('words-native',native,[],hidden)
        assert (result.returncode,result.stdout,result.stderr)==(0,bytes(expected),b''),result
        for name,text in [('copy-arity','print("a".toString(1))\n'),('repeat-missing','print("a".repeat())\n'),
                          ('repeat-type','print("a".repeat("bad"))\n'),('repeat-ref','var n:i64=2\nprint("a".repeat(ref n))\n'),
                          ('copy-typearg','print("a".toString<i64>())\n')]:
            entry=output/(name+'.xr');entry.write_text(text,encoding='utf-8')
            result=invoke(name,binary,['run',str(entry)],hidden)
            assert result.returncode==1 and not result.stdout and result.stderr,result
        poison.unlink();result=invoke('words-source-absent',binary,['run',str(case)],hidden)
        assert (result.returncode,result.stdout,result.stderr)==(0,bytes(expected),b''),result
        (output/'summary.json').write_text(json.dumps(dict(status='STRING_OWNED_ORACLE_PASS',commands=len(records),
            word_count_pairs=len(pairs),UTF8_non_BMP_and_NUL=True,empty_max_count=True,negative_count_empty=True,
            power_two_lengths=[2048,4095,4096,4097],source_poisoned_and_absent=True,VM_C_provider_PATH_hidden=True,
            receiver_snapshot_before_count_effects=True,count_evaluated_once=True,
            CTFE_owning_text=False,complete_string_family=False,complete_standard_library=False),indent=2)+'\n',encoding='utf-8')
        print('Owned string recipes: 64 independent UTF-8/count pairs, empty max, power-two boundaries and native PASS')
        return 0
    finally:
        if temporary: temporary.cleanup()

if __name__=='__main__': raise SystemExit(main())
