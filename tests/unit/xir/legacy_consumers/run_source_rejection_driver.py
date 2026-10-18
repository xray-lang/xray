"""Qualify one exact checked rejection with its real SDK owner and full controls."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,time
import task_rejection_model

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
PREFIX = 'tests/unit/xir/legacy_consumers/'
sys.path.insert(0, str(ROOT/'tests/lib'))
from xraytest import proc
digest = lambda data: hashlib.sha256(data).hexdigest()


def checked_owner(binary, owner, manifest_path, product_inputs=None):
    from fixed_source_rejection_owner import FixedSourceOwner
    return FixedSourceOwner(binary,owner,manifest_path,product_inputs).verify()


def run_group(binary, compiler, owner, manifest_path, output, timeout=60, product_inputs=None):
    oracle = task_rejection_model.expected()
    output = Path(output); output.mkdir(parents=True, exist_ok=False)
    result = {'id':oracle['id'], 'oracle_id':oracle['id'], 'source':oracle['source'],
              'source_lf_sha256':oracle['source_lf_sha256'], 'entry':'source_rejection',
              'qualification_scope':oracle['qualification_scope'], 'commands':[], 'routes':{},
              'full_FI_resource_physical_sanitizer':'OPEN', 'production_release':False}
    if owner is None or manifest_path is None or compiler is None:
        result.update(status='FAIL_MISSING_SOURCE_REJECTION_OWNER_OR_PROVIDER')
        (output/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        return result
    binary = Path(binary).resolve(strict=True); owner = Path(owner).resolve(strict=True)
    compiler = Path(compiler).resolve(strict=True); manifest_path = Path(manifest_path).resolve(strict=True)
    manifest_before = manifest_path.read_bytes(); binding = checked_owner(binary, owner, manifest_path,product_inputs)
    files = [oracle['source'], *(item['source'] for item in oracle['controls'])]
    physical = []
    for i,name in enumerate(files):
        path = output/('input'+str(i)+'.xr'); raw = (ROOT/name).read_bytes()
        if digest(raw.replace(b'\r\n',b'\n')) != task_rejection_model._INPUTS[name]:
            raise ValueError('SDK rejection input lost its exact source bytes')
        path.write_bytes(raw); physical.append(path)
    result['physical_sources'] = [{'identity':name, 'physical':str(path), 'sha256':digest(path.read_bytes())}
                                  for name,path in zip(files,physical)]
    environment = dict(os.environ,XRAY_STDLIB_PATH=binding['stdlib_path'],NO_COLOR='1',PYTHONUTF8='1')
    def invoke(label,command):
        begun = time.monotonic(); observed = proc.run(command,cwd=output,env=environment,timeout=timeout)
        (output/(label+'.stdout')).write_bytes(observed.stdout); (output/(label+'.stderr')).write_bytes(observed.stderr)
        result['commands'].append({'label':label,'argv':list(map(str,command)), 'exit_code':observed.returncode,
            'timed_out':observed.timed_out,'seconds':time.monotonic()-begun,
            'stdout_hex':observed.stdout.hex(),'stderr_hex':observed.stderr.hex()})
        return observed
    observed = invoke('sdk-owner',[owner,*physical,Path(binding['stdlib_path'])])
    report = None
    if observed.returncode == 0 and not observed.timed_out and not observed.stderr:
        try:
            report = json.loads(observed.stdout)
            task_rejection_model.validate_report(report,binding['compiler_identities'])
        except (ValueError,TypeError,KeyError): report = None
    result['sdk_report'] = report
    result['routes']['owned_checked_rejection'] = 'PASS' if report else 'FAIL_CONTROLLED_REJECTION'
    for label,command in [
        ('negative-check',[binary,'check',physical[0]]), ('negative-run',[binary,'run',physical[0]]),
        ('negative-emit',[binary,'build','--c-only',physical[0],'-o',output/'negative.c']),
        ('negative-build',[binary,'build',physical[0],'-o',output/'negative.exe','--cc',compiler])]:
        observed = invoke(label,command)
        ok = observed.returncode == 1 and not observed.timed_out and not observed.stdout and bool(observed.stderr)
        result['routes'][label] = 'PASS' if ok else 'FAIL_REQUIRED_CHECKED_REJECTION'
    if (output/'negative.c').exists() or (output/'negative.exe').exists():
        result['routes']['negative-publication'] = 'FAIL_REJECTED_PRODUCT_PUBLISHED'
    else: result['routes']['negative-publication'] = 'PASS'
    for i,control in enumerate(oracle['controls']):
        source = physical[i+1]; c = output/('control'+str(i)+'.c'); native = output/('control'+str(i)+'.exe')
        def matches(value):
            return (not value.timed_out and value.returncode == control['expected']['exit_code']
                    and value.stdout == control['expected']['stdout_utf8'].encode()
                    and value.stderr == control['expected']['stderr_utf8'].encode())
        vm = invoke('control'+str(i)+'-vm',[binary,'run',source])
        emit = invoke('control'+str(i)+'-emit',[binary,'build','--c-only',source,'-o',c])
        generated = c.read_bytes() if c.is_file() else b''
        projection = (not emit.timed_out and emit.returncode == 0 and not emit.stderr
                      and b'const XrXirProgramSpec xray_app_program' in generated and b'XrXirCallEntry' in generated
                      and b'XrProto' not in generated and b'xrt_vm_run' not in generated
                      and not re.search(rb'\(\s*\{',generated))
        build = invoke('control'+str(i)+'-build',[binary,'build',source,'-o',native,'--cc',compiler])
        native_ok = False
        if not build.timed_out and build.returncode == 0 and not build.stderr and native.is_file():
            native_hash = digest(native.read_bytes()); native_ok = matches(invoke('control'+str(i)+'-native',[native]))
            native_ok = native_ok and native_hash == digest(native.read_bytes())
        result['routes']['control'+str(i)+'-VM-C-native'] = 'PASS' if matches(vm) and projection and native_ok else 'FAIL_COMPLETE_CONTROL'
    if (manifest_before != manifest_path.read_bytes() or checked_owner(binary,owner,manifest_path,product_inputs) != binding
            or task_rejection_model.expected() != oracle):
        raise ValueError('SDK rejection inputs changed during qualification')
    result['owner_manifest_sha256'] = digest(manifest_before)
    result['compiler_identities'] = binding['compiler_identities']
    result['status'] = ('PASS_INDEPENDENT_CHECKED_REJECTION_SCOPE'
                        if all(value == 'PASS' for value in result['routes'].values()) else 'FAIL_CONTROLLED_REJECTION')
    (output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return result
