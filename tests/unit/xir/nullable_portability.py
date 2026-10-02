"""Compile actual sum entries as C11 and reject provider-specific expressions."""
from pathlib import Path
import argparse, re, subprocess, tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-compiler',required=True)
    parser.add_argument('--msvc',action='store_true')
    parser.add_argument('--msvc-atomics',action='store_true')
    parser.add_argument('--zig',action='store_true')
    parser.add_argument('--source',type=Path,action='append',required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[3]
    with tempfile.TemporaryDirectory(prefix='xir-sum-c11-') as temporary:
        for source in args.source:
            text=source.read_text(encoding='utf-8')
            assert 'xr_xir_nullable_new' in text and not re.search(r'\(\s*\{',text)
            output=Path(temporary)/(source.stem+'.obj')
            if args.msvc:
                command=[args.host_compiler,'/nologo','/std:c11','/utf-8',
                         '/D_CRT_SECURE_NO_WARNINGS','/W4','/WX','/I'+str(root/'src'),
                         '/I'+str(root/'include'),'/c',str(source),'/Fo'+str(output)]
                if args.msvc_atomics:command.append('/experimental:c11atomics')
            else:
                command=[args.host_compiler,*(['cc'] if args.zig else []),'-std=c11','-Wall','-Wextra',
                         '-Werror','-pedantic-errors','-isystem',str(root/'src'),'-isystem',str(root/'include'),
                         '-c',str(source),'-o',str(output)]
            result=subprocess.run(command,capture_output=True,timeout=90)
            if result.returncode:
                print((result.stdout+result.stderr).decode('utf-8',errors='replace'))
                return result.returncode
            assert output.exists() and output.stat().st_size>0
            print(source.name+': actual generated C11 object PASS',flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())
