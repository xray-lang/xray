"""Build and bind the actual SDK rejection consumer to one fresh public product."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,os,re,subprocess,sys

PREFIX = 'tests/unit/xir/legacy_consumers/'
digest = lambda data: hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--consumer-root', type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--archives', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.source_root.resolve(strict=True); consumer = args.consumer_root.resolve(strict=True)
    binary = args.binary.resolve(strict=True); archives = args.archives.resolve(strict=True)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    archive_bytes = archives.read_bytes()
    paths = [Path(name).resolve(strict=True) for name in re.findall(r'"([^"\n]+\.lib)"', archive_bytes.decode('utf-8'))]
    if len(paths) != 15 or len(set(paths)) != 15 or not all(path.is_relative_to(binary.parent) for path in paths):
        raise ValueError('SDK owner needs the exact fifteen archives from the selected public product build')
    before_archives = {str(path): digest(path.read_bytes()) for path in paths}
    def physical(relative):
        selected = consumer / relative[len(PREFIX):] if relative.startswith(PREFIX) else root / relative
        return selected if selected.is_file() else root / relative
    spec = importlib.util.spec_from_file_location('bound_task_rejection_model', consumer / 'task_rejection_model.py')
    model = importlib.util.module_from_spec(spec); spec.loader.exec_module(model)
    class SourceView:
        def __truediv__(self, relative): return physical(str(relative))
    model.ROOT = SourceView(); model.expected()
    inputs = {name: digest(physical(name).read_bytes()) for name in model._INPUTS}
    inputs[PREFIX+'task_rejection_model.py'] = digest((consumer/'task_rejection_model.py').read_bytes())
    inputs[PREFIX+'build_source_rejection_owner.py'] = digest((consumer/'build_source_rejection_owner.py').read_bytes())
    version = json.loads(subprocess.check_output([str(binary), '--version', '--json'], text=True))
    head = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    if version['commit'] != head or version['dirty'] or subprocess.check_output(
            ['git', '-C', str(root), 'status', '--porcelain'], text=True).strip():
        raise ValueError('SDK owner requires the freshly built clean current public product')
    binary_sha = digest(binary.read_bytes()); commands = []
    def run(label, command):
        result = subprocess.run(command, capture_output=True, timeout=60,
                                env=dict(os.environ, PYTHONUTF8='1', NO_COLOR='1'))
        (output/(label+'.stdout')).write_bytes(result.stdout); (output/(label+'.stderr')).write_bytes(result.stderr)
        commands.append({'argv':command, 'exit_code':result.returncode,
                         'stdout_hex':result.stdout.hex(), 'stderr_hex':result.stderr.hex()})
        if result.returncode:
            raise RuntimeError('SDK rejection consumer '+label+' failed: '+result.stderr.decode('utf-8', errors='replace'))
    build = output/'build'
    run('configure', ['cmake', '-S', str(consumer/'source_rejection_owner'), '-B', str(build), '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=Release', '-DXRAY_SOURCE_ROOT='+str(root), '-DXRAY_PRODUCER_ARCHIVES='+str(archives)])
    run('build', ['cmake', '--build', str(build), '--parallel', '2'])
    owner = build/'legacy_source_rejection_owner.exe'
    if not owner.is_file(): raise ValueError('The freshly linked SDK rejection owner is missing')
    faults=build/'legacy_source_rejection_faults.exe'
    if not faults.is_file():raise ValueError('The complete compiler fault owner is missing')
    if (binary_sha != digest(binary.read_bytes()) or archives.read_bytes() != archive_bytes
            or before_archives != {str(path):digest(path.read_bytes()) for path in paths}
            or any(digest(physical(name).read_bytes()) != value for name,value in inputs.items())
            or subprocess.check_output(['git','-C',str(root),'status','--porcelain'],text=True).strip()):
        raise ValueError('SDK rejection build inputs changed')
    def macro(path, name):
        match = re.search(r'^#define '+name+r' (\d+)u?\s*$', (root/path).read_text(encoding='utf-8'), re.MULTILINE)
        if not match: raise ValueError('SDK compiler identity has no reviewed literal: '+name)
        return int(match[1])
    identities = {'checked_schema':macro('src/xir/xxir_checked.h','XR_XIR_CHECKED_SCHEMA'),
                  'semantic_identity':macro('src/xir/xxir_checked.h','XR_XIR_CHECKED_CONTRACT'),
                  'value_abi':macro('src/xir/xxir_value.h','XR_XIR_VALUE_ABI_VERSION')}
    manifest = {'schema':2, 'source_root':str(root), 'code_head':head, 'cli_binary':str(binary),
                'cli_sha256':binary_sha, 'cli_version':version, 'owner_binary':str(owner),
                'owner_sha256':digest(owner.read_bytes()), 'fault_owner_binary':str(faults),
                'fault_owner_sha256':digest(faults.read_bytes()), 'producer_archives':before_archives,
                'archive_manifest':str(archives), 'archive_manifest_sha256':digest(archive_bytes),
                'inputs':inputs, 'compiler_identities':identities, 'commands':commands,
                'qualification':'BUILT_INPUTS_BOUND_NOT_EXECUTION_OR_FULL_SAFETY', 'production_release':False}
    (output/'owner-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'owner':str(owner), 'manifest':str(output/'owner-manifest.json'),
                      'producer_archives':15, 'code_head':head, 'qualification':manifest['qualification']}))
    return 0


if __name__ == '__main__': raise SystemExit(main())
