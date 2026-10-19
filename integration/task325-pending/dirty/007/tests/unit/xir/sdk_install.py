"""Install and admit the exact same runtime bundle without rebuilding its contents."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--prefix',type=Path,required=True)
    args=parser.parse_args();build=args.build.resolve(strict=True)
    assert args.prefix.is_absolute(), 'The owned install prefix must be absolute'
    prefix=args.prefix.resolve()
    assert prefix!=build and prefix.is_relative_to(build), 'The owned install prefix must be inside this build'
    assert not prefix.exists() and not prefix.is_symlink(), 'The owned install prefix must not exist'
    original=build/'xir-runtime-sdk'
    manifest=json.loads((original/'sdk_manifest.json').read_text(encoding='utf-8'))
    recipe=json.loads((original/'sdk_recipe.json').read_text(encoding='utf-8'))
    assert [row['path'] for row in manifest['files']]==[row['path'] for row in recipe['files']]
    installed=prefix/'lib/xray/xir-runtime-sdk'
    expected_manifest=os.linesep.join((installed/row['path']).as_posix() for row in manifest['files'])
    expected_manifest+=os.linesep+(installed/'sdk_manifest.json').as_posix()
    prefix.mkdir(parents=True,exist_ok=False)
    try:
        result=subprocess.run(['cmake','--install',str(build),'--prefix',prefix.as_posix(),
                               '--component','XraySDK'],capture_output=True,timeout=120)
        assert not result.returncode,(result.stdout+result.stderr).decode('utf-8',errors='replace')
        assert (build/'install_manifest_XraySDK.txt').read_bytes()==expected_manifest.encode('utf-8')
        assert (installed/'sdk_manifest.json').read_bytes()==(original/'sdk_manifest.json').read_bytes()
        paths={row['path'] for row in manifest['files']}|{'sdk_manifest.json'}
        assert {path.relative_to(installed).as_posix() for path in installed.rglob('*') if path.is_file()}==paths
        for row in manifest['files']:
            data=(installed/row['path']).read_bytes()
            assert len(data)==row['length'] and hashlib.sha256(data).hexdigest()==row['sha256']
        admitted=subprocess.run([args.owner,str(installed),str(installed/'sdk_manifest.json'),'status'],
                                capture_output=True,timeout=30)
        assert not admitted.returncode,admitted.stderr.decode('utf-8',errors='replace')
        facts=admitted.stdout.decode('utf-8').split();assert facts[:2]==['0',str(len(manifest['files']))],facts
    finally:
        assert not prefix.is_symlink() and prefix.resolve()==prefix and prefix.is_relative_to(build)
        shutil.rmtree(prefix)
        assert not prefix.exists()
    print('Exact '+str(len(manifest['files']))+' files installed; same compiled admission facts PASS')


if __name__=='__main__':main()
