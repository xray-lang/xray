"""Install and admit the exact same runtime bundle without rebuilding its contents."""
from pathlib import Path
import argparse, hashlib, json, subprocess, tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True);parser.add_argument('--owner',required=True)
    args=parser.parse_args();original=args.build/'xir-runtime-sdk'
    manifest=json.loads((original/'sdk_manifest.json').read_text())
    with tempfile.TemporaryDirectory(prefix='xir-sdk-installed-') as temporary:
        prefix=Path(temporary)
        result=subprocess.run(['cmake','--install',str(args.build),'--prefix',str(prefix),
                               '--component','XraySDK'],capture_output=True,timeout=120)
        assert not result.returncode,(result.stdout+result.stderr).decode(errors='replace')
        installed=prefix/'lib/xray/xir-runtime-sdk'
        assert (installed/'sdk_manifest.json').read_bytes()==(original/'sdk_manifest.json').read_bytes()
        paths={row['path'] for row in manifest['files']}|{'sdk_manifest.json'}
        assert {path.relative_to(installed).as_posix() for path in installed.rglob('*') if path.is_file()}==paths
        for row in manifest['files']:
            data=(installed/row['path']).read_bytes()
            assert len(data)==row['length'] and hashlib.sha256(data).hexdigest()==row['sha256']
        admitted=subprocess.run([args.owner,str(installed),str(installed/'sdk_manifest.json'),'status'],
                                capture_output=True,timeout=30)
        assert not admitted.returncode,admitted.stderr.decode(errors='replace')
        facts=admitted.stdout.decode().split();assert facts[:2]==['0',str(len(manifest['files']))],facts
    print('Exact '+str(len(manifest['files']))+' files installed; same compiled admission facts PASS')


if __name__=='__main__':main()
