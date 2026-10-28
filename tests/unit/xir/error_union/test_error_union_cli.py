"""Execute complete canonical Error unions through the actual CLI and native SDK."""
from pathlib import Path
import argparse
import hashlib
import json
import sys
import tempfile


def source_300():
    text=''.join(f'enum E{i}{{Bad}}\n' for i in range(300))
    text+='fn value() throws('+'|'.join(f'E{i}' for i in range(300))+')->i64{throw E299.Bad}\n'
    text+='export fn answer()->i64{try{return value()}catch(e:E299){return 42}catch(e){return -1}}\nprint(answer())\n'
    assert len(text.encode())==5924
    return text.encode()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--expect',type=Path,required=True)
    args=parser.parse_args()
    sys.path.insert(0,str(args.root.resolve()/'scripts'))
    from xray_process import run_child
    binary=args.binary.resolve();source=args.source.read_bytes()
    oracle=json.loads(args.expect.read_text(encoding='utf-8'))
    assert len(source)==oracle['sourceBytes'] and hashlib.sha256(source).hexdigest()==oracle['sourceSHA256']
    expected=oracle['stdoutUtf8'].encode('utf-8')
    with tempfile.TemporaryDirectory(prefix='xray-error-union-') as temporary:
        directory=Path(temporary)

        def run(command):
            with tempfile.TemporaryFile() as stdout,tempfile.TemporaryFile() as stderr:
                code=run_child(command,cwd=directory,stdout=stdout,stderr=stderr,timeout=120)
                stdout.seek(0);actual=stdout.read();stderr.seek(0);errors=stderr.read()
            return code,actual,errors

        def check(name,command,wanted):
            code,actual,errors=run(command)
            assert code==0 and actual==wanted and not errors,(name,command,code,actual,errors,wanted)
            print(f'{name}: exit0 stdout{len(actual)} raw bytes stderr0')

        for name,data in [('members7',source),('members300',source_300())]:
            path=directory/(name+'.xr');path.write_bytes(data);executable=directory/(name+'.exe')
            check(name+' check',[str(binary),'check',str(path)],b'')
            check(name+' run',[str(binary),'run',str(path)],expected)
            check(name+' build',[str(binary),'build',str(path),'-o',str(executable)],('Built: '+str(executable)+'\n').encode())
            assert executable.is_file();path.unlink()
            check(name+' native producer removed',[str(executable)],expected)

        library=directory/'secret.xr';entry=directory/'access.xr'
        library.write_text('export enum Visible{Bad}\nexport enum Hidden{Bad}\n',encoding='utf-8')
        entry.write_text('import "./secret" as Secret\nfn allowed() throws(Error|Secret.Hidden)->i64{return 0}\n',encoding='utf-8')
        check('exported Error member before absorption',[str(binary),'check',str(entry)],b'')
        library.write_text('export enum Visible{Bad}\nenum Hidden{Bad}\n',encoding='utf-8')
        code,actual,errors=run([str(binary),'check',str(entry)])
        assert code!=0 and not actual and b'import requires an exported declaration' in errors,(code,actual,errors)
        for text,reason in [('fn bad() throws(Error|i64)->i64{return 0}\n',b'satisfy Error'),
                            ('fn bad<E>() throws(Error|E)->i64{return 0}\n',b'satisfy Error')]:
            entry.write_text(text,encoding='utf-8');code,actual,errors=run([str(binary),'check',str(entry)])
            assert code!=0 and not actual and reason in errors,(code,actual,errors)
    print('Actual CLI complete 7/300, native without Source and access/Error proof before absorption PASS')


if __name__=='__main__':main()
