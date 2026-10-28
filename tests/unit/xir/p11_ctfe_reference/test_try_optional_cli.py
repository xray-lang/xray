"""Exercise the complete optional-error fixture through the actual CLI and native SDK."""
from pathlib import Path
import argparse
import json
import os
import sys
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--expect',type=Path,required=True)
    parser.add_argument('--native-driver',type=Path)
    args=parser.parse_args()
    sys.path.insert(0,str(args.root.resolve()/'scripts'))
    from xray_process import run_child
    binary=args.binary.resolve();source=args.source.resolve()
    expected=json.loads(args.expect.read_text(encoding='utf-8'))['stdoutUtf8'].encode('utf-8')
    with tempfile.TemporaryDirectory(prefix='xray-try-optional-cli-') as temporary:
        directory=Path(temporary);executable=directory/'try_optional.exe'
        def check(name,command,wanted):
            with tempfile.TemporaryFile() as stdout,tempfile.TemporaryFile() as stderr:
                status=run_child(command,cwd=directory,stdout=stdout,stderr=stderr,timeout=120)
                stdout.seek(0);actual=stdout.read();stderr.seek(0);errors=stderr.read()
            if status!=0 or actual!=wanted or errors:
                raise AssertionError((name,command,status,actual,errors,wanted))
            print(name+': exit0, stdout '+str(len(actual))+' raw bytes, stderr0')
        if args.native_driver:
            oracle=directory/'expected.stdout';oracle.write_bytes(expected)
            check('generated native',[str(args.native_driver.resolve()),'--expect',str(oracle)],b'Strict fromUtf8 generated C: owned Unicode/NUL and typed catch PASS'+os.linesep.encode())
            return
        check('run',[str(binary),'run',str(source)],expected)
        check('build',[str(binary),'build',str(source),'-o',str(executable)],
            ('Built: '+str(executable)+'\n').encode('utf-8'))
        assert executable.is_file()
        check('native',[str(executable)],expected)
    print('Actual CLI run/build/native complete try? Unicode/NUL oracle PASS')


if __name__=='__main__':main()
