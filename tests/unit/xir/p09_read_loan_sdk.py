"""Compile the shared read-root consumer against an authenticated SDK lease."""
from pathlib import Path
import argparse
import json
import subprocess
import sys
import tempfile

from sdk_native_consumers import checked, compile_source, link


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('lease','bundle','compiler','linker'):
        parser.add_argument('--'+name,required=True)
    parser.add_argument('--provider',choices=('msvc','clang','zig'),required=True)
    args=parser.parse_args();root=Path(__file__).resolve().parents[3]
    bundle=Path(args.bundle)
    lease=subprocess.Popen([args.lease,str(bundle),str(bundle/'sdk_manifest.json')],
        stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    try:
        ready=lease.stdout.readline().strip().split()
        assert ready[:3]==['READY','25','2'],ready
        sdk=Path(lease.stdout.readline().strip())
        libraries=[lease.stdout.readline().strip() for _ in range(5)]
        assert sdk.is_dir() and all(Path(p).is_file() for p in libraries)
        with tempfile.TemporaryDirectory(prefix='xir-sdk-read-loan-') as temporary:
            work=Path(temporary)
            sys.path.insert(0,str(root/'scripts'))
            from derive_xir_sdk_abi import prepare
            proof=work/'abi';prepare(proof)
            probe_object=proof/'abi.obj';probe_exe=proof/'abi.exe'
            compile_source(args,sdk,proof/'abi_probe.c',probe_object)
            link(args,probe_exe,[str(probe_object)],libraries)
            expected=json.loads((proof/'EXPECTED.json').read_text())['rows']
            actual=json.loads(checked([str(probe_exe)]))
            assert actual==[dict(id=row['id'],value=row['value']) for row in expected]
            source=root/'tests/unit/xir/xir_sdk_read_loan_consumer.c'
            object_file=work/'read-loan.obj';executable=work/'read-loan.exe'
            compile_source(args,sdk,source,object_file)
            link(args,executable,[str(object_file)],libraries)
            assert checked([str(executable)]).replace(b'\r\n',b'\n')==b'read-loan-sdk-ok\n'
            print(args.provider+': shared read root, non-LIFO release, COW and '+str(len(expected))+' independent ABI fields PASS',flush=True)
    finally:
        if lease.poll() is None:
            lease.stdin.write('q');lease.stdin.flush()
        _,error=lease.communicate(timeout=10)
        assert not lease.returncode,error


if __name__=='__main__':
    main()
