"""Complete EN/CN README task examples through current VM, C and formatter."""
from pathlib import Path
import argparse, hashlib, json, os, sys, tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();root=args.root.resolve()
    sys.path.insert(0,str(root/'scripts'));sys.path.insert(0,str(root/'tests/smoke'))
    from xray_process import run_child
    from run_smoke import readme_blocks
    args.output.mkdir(parents=True,exist_ok=True)
    out=Path(tempfile.mkdtemp(prefix='run-',dir=args.output));commands=[];inputs=[]
    environment=dict(os.environ,XRAY_STDLIB_PATH=str(root/'stdlib'),NO_COLOR='1',PYTHONUTF8='1')
    def run(name,argv,expected=None):
        argv=list(map(str,argv));stdout,stderr=out/(name+'.stdout'),out/(name+'.stderr')
        with stdout.open('wb') as a,stderr.open('wb') as b:
            code=run_child(argv,cwd=root,env=environment,stdout=a,stderr=b,timeout=30)
        commands.append(dict(name=name,argv=argv,exit=code))
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        assert code==0,(name,code,stdout.read_bytes(),stderr.read_bytes())
        if expected is not None:assert stdout.read_bytes()==expected and not stderr.read_bytes(),name
    for index,name in enumerate(('README.md','README_CN.md')):
        text=readme_blocks((root/name).read_text(encoding='utf-8'))[0]
        for required in ('go count(copy first)','go count(copy second)','try await left','try await right'):
            assert text.count(required)==1,(name,required)
        source=out/f'readme-{index}.xr';source.write_text(text,encoding='utf-8',newline='\n')
        inputs.append(dict(path=name+'#xray-1',bytes=len(source.read_bytes()),sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
        run(f'vm-{index}',[args.binary,'run',source],b'21\n')
        exe=out/f'readme-{index}.exe'
        run(f'build-{index}',[args.binary,'build',source,'--native','-o',exe])
        run(f'native-{index}',[exe],b'21\n')
        formatted=out/f'formatted-{index}.xr';formatted.write_bytes(source.read_bytes())
        run(f'format-{index}',[args.binary,'fmt',formatted])
        assert 'unsupported expr' not in formatted.read_text(encoding='utf-8')
        run(f'formatted-vm-{index}',[args.binary,'run',formatted],b'21\n')
    (out/'result.json').write_text(json.dumps(dict(state='PASS',inputs=inputs,commands=commands),indent=2)+'\n')
    print('Complete English/Chinese README copy/task examples: VM/native21LF and format roundtrip PASS')

if __name__=='__main__':main()
