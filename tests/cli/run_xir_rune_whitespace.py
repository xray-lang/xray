"""One exact Rune predicate through current Source, VM, CTFE and generated C."""
from pathlib import Path
import argparse,json,os,sys,tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();root=args.root.resolve()
    sys.path.insert(0,str(root/'scripts'))
    from xray_process import run_child
    args.output.mkdir(parents=True,exist_ok=True)
    output=Path(tempfile.mkdtemp(prefix='run-',dir=args.output))
    environment=dict(os.environ,XRAY_STDLIB_PATH=str(root/'stdlib'),NO_COLOR='1',PYTHONUTF8='1')
    commands=[]
    def run(name,command,success=True,expected=None):
        command=list(map(str,command))
        stdout,stderr=output/(name+'.stdout'),output/(name+'.stderr')
        with stdout.open('wb') as out,stderr.open('wb') as err:
            code=run_child(command,cwd=root,env=environment,stdout=out,stderr=err,timeout=30)
        commands.append(dict(name=name,argv=command,exit=code))
        (output/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        assert (code==0)==success,(name,code,stdout.read_bytes(),stderr.read_bytes())
        if expected is not None:assert stdout.read_bytes()==expected and not stderr.read_bytes(),name
    positives=[9,10,11,12,13,32,0xa0,0x1680,0x2000,0x2005,0x200a,0x2028,0x2029,0x202f,0x205f,0x3000]
    negatives=[0,8,14,33,0x7f,0x85,0x200b,0xfeff,0x1f600,0x10ffff]
    source='fn white(value:rune)->bool { return value.isWhitespace() }\n'
    for scalar in positives:source+=f'assert(white(rune({scalar})!))\n'
    for scalar in negatives:source+=f'assert(!white(rune({scalar})!))\n'
    source+='var folded = comptime { rune(12288)!.isWhitespace() }\nassert(folded)\n'
    source+='var changing:rune = rune(32)!\nassert(changing.isWhitespace())\n'
    source+='changing = rune(65)!\nassert(!changing.isWhitespace())\nprint(42)\n'
    path=output/'whole.xr';path.write_text(source,encoding='utf-8',newline='\n')
    run('check',[args.binary,'check',path])
    run('vm',[args.binary,'run',path],expected=b'42\n')
    exe=output/'whole.exe'
    run('build-native',[args.binary,'build',path,'--native','-o',exe])
    run('native',[exe],expected=b'42\n')
    rejected={
        'integer-receiver':'print((32).isWhitespace())\n',
        'string-receiver':'print(" ".isWhitespace())\n',
        'extra-argument':'print(rune(32)!.isWhitespace(1))\n',
        'type-argument':'print(rune(32)!.isWhitespace<i64>())\n',
        'wrong-result':'fn f(r:rune)->i64 { return r.isWhitespace() }\nprint(f(rune(32)!))\n',
        'letter-still-closed':'print(rune(65)!.isLetter())\n',
        'number-still-closed':'print(rune(65)!.isNumber())\n',
        'alphanumeric-still-closed':'print(rune(65)!.isAlphanumeric())\n'}
    for name,text in rejected.items():
        path=output/(name+'.xr');path.write_text(text,encoding='utf-8',newline='\n')
        for mode in ['check','run','build']:
            command=[args.binary,mode,path]
            target=output/(name+'.exe')
            if mode=='build':command+=['--native','-o',target]
            run(name+'-'+mode,command,False)
            assert not target.exists(),name
    print('Rune whitespace exact Source/VM/CTFE/C and closed unrelated methods PASS')

if __name__=='__main__':main()
