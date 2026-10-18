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
        assert ready[:3]==['READY','27','2'],ready
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
            for name,expected_line in [('read_loan',b'read-loan-sdk-ok\n'),('view_types',b'view-types-sdk-ok\n'),
                                       ('view_storage',b'view-storage-sdk-ok\n'),('view_values',b'view-values-sdk-ok\n'),
                                       ('view_retire',b'view-retire-sdk-ok\n'),('return_move',b'return-move-sdk-ok\n'),
                                       ('view_origin_pin',b'view-origin-pin-sdk-ok\n'),('call_views',b'call-views-sdk-ok\n')]:
                source=root/('tests/unit/xir/xir_sdk_'+name+'_consumer.c')
                object_file=work/(name+'.obj');executable=work/(name+'.exe')
                compile_source(args,sdk,source,object_file)
                link(args,executable,[str(object_file)],libraries)
                assert checked([str(executable)]).replace(b'\r\n',b'\n')==expected_line
            print(args.provider+': shared read root, non-LIFO release, COW, borrowed descriptors, actual Call scope retirement and '+str(len(expected))+' independent ABI fields PASS',flush=True)
    finally:
        if lease.poll() is None:
            lease.stdin.write('q');lease.stdin.flush()
        _,error=lease.communicate(timeout=10)
        assert not lease.returncode,error


if __name__=='__main__':
    main()
