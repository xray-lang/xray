"""Install and admit the exact same runtime bundle without rebuilding its contents."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True);parser.add_argument('--owner',required=True)
    parser.add_argument('--prefix',type=Path,required=True)
    parser.add_argument('--source-sdk',action='store_true')
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
    source_original=build/'source-sdk';source_installed=prefix/'lib/xray/source-sdk'
    source_manifest=None
    expected_files={installed/row['path'] for row in manifest['files']}|{installed/'sdk_manifest.json'}
    if args.source_sdk:
        source_manifest=json.loads((source_original/'source_sdk_manifest.json').read_text(encoding='utf-8'))
        members=source_manifest['archives']+['include/xray_source.h','include/xray_export.h',
                                           'source_sdk_manifest.json','source_sdk_recipe.json']
        assert len(members)==len(set(members))
        assert set(members)=={row['path'] for row in source_manifest['files']}|{'source_sdk_manifest.json'}
        configuration=prefix/'lib/cmake/XraySource/XraySourceConfig.cmake'
        expected_manifest+=os.linesep+os.linesep.join((source_installed/name).as_posix() for name in members)
        expected_manifest+=os.linesep+configuration.as_posix()
        expected_files.update(source_installed/name for name in members)
        expected_files.add(configuration)
    payload_path=prefix/'share/xray/install/payload-manifest.json'
    expected_files.add(payload_path)
    prefix.mkdir(parents=True,exist_ok=False)
    try:
        result=subprocess.run(['cmake','--install',str(build),'--prefix',prefix.as_posix(),
                               '--component','XraySDK'],capture_output=True,timeout=120)
        assert not result.returncode,(result.stdout+result.stderr).decode('utf-8',errors='replace')
        assert (build/'install_manifest_XraySDK.txt').read_bytes()==expected_manifest.encode('utf-8')
        actual_files={path for path in prefix.rglob('*') if path.is_file()}
        assert actual_files==expected_files,(actual_files-expected_files,expected_files-actual_files)
        payload=json.loads(payload_path.read_text(encoding='utf-8'))
        assert payload['schema']==2 and payload['product']=='xray-lang' and payload['components']==['sdk']
        assert [row['path'] for row in payload['files']]==sorted(path.relative_to(prefix).as_posix()
            for path in expected_files-{payload_path})
        for row in payload['files']:
            assert set(row)=={'path','component','size','sha256','mode'} and row['component']=='sdk' and row['mode']=='0644'
            path=prefix/row['path'];assert not path.is_symlink()
            data=path.read_bytes()
            assert len(data)==row['size'] and hashlib.sha256(data).hexdigest()==row['sha256']
        assert (installed/'sdk_manifest.json').read_bytes()==(original/'sdk_manifest.json').read_bytes()
        paths={row['path'] for row in manifest['files']}|{'sdk_manifest.json'}
        assert {path.relative_to(installed).as_posix() for path in installed.rglob('*') if path.is_file()}==paths
        for row in manifest['files']:
            data=(installed/row['path']).read_bytes()
            assert len(data)==row['length'] and hashlib.sha256(data).hexdigest()==row['sha256']
        if source_manifest is not None:
            assert (source_installed/'source_sdk_manifest.json').read_bytes()==(source_original/'source_sdk_manifest.json').read_bytes()
            assert configuration.read_bytes()==(source_original/'XraySourceConfig.cmake').read_bytes()
            for row in source_manifest['files']:
                data=(source_installed/row['path']).read_bytes()
                assert len(data)==row['length'] and hashlib.sha256(data).hexdigest()==row['sha256']
        admitted=subprocess.run([args.owner,str(installed),str(installed/'sdk_manifest.json'),'status'],
                                capture_output=True,timeout=30)
        assert not admitted.returncode,admitted.stderr.decode('utf-8',errors='replace')
        facts=admitted.stdout.decode('utf-8').split();assert facts[:2]==['0',str(len(manifest['files']))],facts
    finally:
        assert not prefix.is_symlink() and prefix.resolve()==prefix and prefix.is_relative_to(build)
        shutil.rmtree(prefix)
        assert not prefix.exists()
    print('Exact '+str(len(expected_files))+' component files installed; same compiled admission facts PASS')


if __name__=='__main__':main()
