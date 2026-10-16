"""Build one current canonical CLI Source owner against its complete fixed build."""
from pathlib import Path
import argparse,json,os,subprocess
from fixed_product_inputs import FixedProductInputs
import fixed_source_rejection_owner as binding


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--producer-build',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--product-inputs',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();root=args.source_root.resolve(strict=True);build=args.producer_build.resolve(strict=True)
    binary=args.binary.resolve(strict=True);fixed=FixedProductInputs(args.product_inputs,binary)
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    version=json.loads(subprocess.check_output([str(binary),'--version','--json'],text=True,timeout=30));fixed.require_version(version)
    if binding.git(root,'status','--porcelain') or binding.git(binding.ROOT,'status','--porcelain') or binding.git(root,'rev-parse','HEAD')!=fixed.manifest['source_commit']:
        raise ValueError('Source owner needs a clean fixed producer and clean consumer')
    if binding.digest(binary.read_bytes())!=binding.digest((build/'xray.exe').read_bytes()):
        raise ValueError('Public CLI was not copied from this exact producer build')
    binding.task_rejection_model.expected()
    consumers=binding.file_table(binding.ROOT,binding.consumer_names());producer=binding.file_table(root,binding.producer_names(root))
    graph=binding.file_table(build,binding.BUILD_FILES);links=binding.link_inputs(build)
    archives=output/'producer-archives.cmake'
    archives.write_text('set(XR_PRODUCER_ARCHIVES\n'+''.join('    "'+Path(p).as_posix()+'"\n' for p in links['archives'])+''.join('    "'+p+'"\n' for p in links['system_libraries'])+')\n',encoding='utf-8',newline='\n')
    commands=[]
    def run(label,argv):
        result=subprocess.run(argv,capture_output=True,timeout=60,env=dict(os.environ,PYTHONUTF8='1',NO_COLOR='1'))
        (output/(label+'.stdout')).write_bytes(result.stdout);(output/(label+'.stderr')).write_bytes(result.stderr)
        commands.append({'argv':argv,'exit_code':result.returncode,'stdout_hex':result.stdout.hex(),'stderr_hex':result.stderr.hex()})
        if result.returncode:raise RuntimeError('Canonical Source owner '+label+' failed: '+result.stderr.decode('utf-8',errors='replace'))
    owned=output/'build'
    run('configure',['cmake','-S',str(binding.HERE/'source_rejection_owner'),'-B',str(owned),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DXRAY_SOURCE_ROOT='+str(root),'-DXRAY_PRODUCER_ARCHIVES='+str(archives)])
    run('build',['cmake','--build',str(owned),'--parallel','2'])
    owner=owned/'legacy_source_rejection_owner.exe';faults=owned/'legacy_source_rejection_faults.exe'
    m={'schema':3,'kind':'CANONICAL_CLI_SOURCE_REJECTION_OWNER','source_root':str(root),'code_head':version['commit'],'consumer_root':str(binding.ROOT.resolve()),'consumer_head_at_build':binding.git(binding.ROOT,'rev-parse','HEAD'),'producer_build':str(build),'cli_binary':str(binary),'cli_version':version,'cli_sha256':binding.digest(binary.read_bytes()),'product_inputs_manifest':str(fixed.manifest_path),'product_inputs_manifest_sha256':binding.digest(fixed.raw),'stdlib_path':str(fixed.stdlib),'owner_binary':str(owner),'owner_sha256':binding.digest(owner.read_bytes()),'fault_owner_binary':str(faults),'fault_owner_sha256':binding.digest(faults.read_bytes()),'consumer_inputs':consumers,'producer_inputs':producer,'producer_build_inputs':graph,'link_inputs':links,'compiler_identities':binding.compiler_identities(root),'commands':commands,'qualification':'BUILT_INPUTS_BOUND_NOT_EXECUTION_OR_FULL_SAFETY','installed_SDK_source_API_qualification':False,'production_release':False}
    path=output/'owner-manifest.json';path.write_text(json.dumps(m,indent=2)+'\n',encoding='utf-8',newline='\n')
    binding.FixedSourceOwner(binary,owner,path,fixed).verify()
    print(json.dumps({'owner':str(owner),'manifest':str(path),'producer_archives':len(links['archives']),'code_head':m['code_head'],'qualification':m['qualification']}))
    return 0


if __name__=='__main__':raise SystemExit(main())
