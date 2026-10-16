"""Bind a canonical CLI Source consumer to one separate fixed product and build."""
from pathlib import Path
import hashlib,json,re,subprocess
from fixed_product_inputs import FixedProductInputs, _object, _linked
import task_rejection_model

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[3]
PREFIX='tests/unit/xir/legacy_consumers/'
BUILD_FILES=('CMakeCache.txt','build.ninja','CMakeFiles/rules.ninja','compile_commands.json')
digest=lambda value:hashlib.sha256(value).hexdigest()


def git(root,*args):
    return subprocess.check_output(['git','-C',str(root),*args],text=True,encoding='utf-8').strip()


def consumer_names():
    return set(task_rejection_model._INPUTS)|{PREFIX+'task_rejection_model.py',PREFIX+'build_source_rejection_owner.py',PREFIX+'fixed_source_rejection_owner.py',PREFIX+'fixed_product_inputs.py'}


def producer_names(root):
    return git(root,'ls-files','--','src','include','cmake','scripts','CMakeLists.txt',
               'tests/unit/xir/xir_instance_compile_observer.h').splitlines()


def file_table(root,names):
    result={}
    for name in names:
        path=root/name
        if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root.resolve()):
            raise ValueError('Source owner input must be an ordinary owned file')
        result[name]=digest(path.read_bytes())
    return result


def link_inputs(build):
    commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','xray'],text=True,encoding='utf-8').splitlines()
    command=commands[-1]
    if '/subsystem:console' not in command:raise ValueError('Expected the actual MSVC public CLI link command')
    libraries=re.findall(r'(?<![:\w])([\w\\./-]+\.lib)\b',command.split('/subsystem:console',1)[1])
    archives={};system=[]
    for name in libraries:
        path=(build/name.replace('\\','/')).resolve()
        if path.is_file():
            if not path.is_relative_to(build.resolve()):raise ValueError('Compiler archive escapes its actual build')
            archives[str(path)]=digest(path.read_bytes())
        else:
            if not re.fullmatch(r'[a-z0-9_]+\.lib',name):raise ValueError('Unresolved non-system compiler archive')
            if name not in system:system.append(name)
    names={Path(name).name for name in archives}
    if not {'xray_cli_source.lib','xray_xir_source_product.lib','xray_compiler_parser.lib'}<=names:
        raise ValueError('The complete canonical CLI Source compiler closure is absent')
    return {'command':command,'archives':archives,'system_libraries':system}


def compiler_identities(root):
    identities={}
    for field,name,path in [('checked_schema','XR_XIR_CHECKED_SCHEMA','src/xir/xxir_checked.h'),
                            ('semantic_identity','XR_XIR_CHECKED_CONTRACT','src/xir/xxir_checked.h'),
                            ('value_abi','XR_XIR_VALUE_ABI_VERSION','src/xir/xxir_value.h')]:
        match=re.search(r'^#define '+name+r' (\d+)u?\s*$',(root/path).read_text(encoding='utf-8'),re.MULTILINE)
        if not match:raise ValueError('Compiler identity has no owned literal')
        identities[field]=int(match[1])
    return identities


class FixedSourceOwner:
    def __init__(self,binary,owner,manifest_path,product_inputs=None):
        if any(_linked(Path(path)) for path in (manifest_path,owner,binary)):raise ValueError('Source owner aliases are not admitted')
        self.path=Path(manifest_path).resolve(strict=True);self.raw=self.path.read_bytes()
        self.manifest=json.loads(self.raw,object_pairs_hook=_object);m=self.manifest
        if type(m.get('schema')) is not int or m['schema']!=3 or m.get('kind')!='CANONICAL_CLI_SOURCE_REJECTION_OWNER':
            raise ValueError('Expected the current separate-product Source owner schema')
        self.product=product_inputs or FixedProductInputs(Path(m['product_inputs_manifest']),Path(binary))
        if self.product.manifest_path!=Path(m['product_inputs_manifest']).resolve(strict=True):
            raise ValueError('Source owner and caller selected different product inputs')
        self.binary=Path(binary).resolve(strict=True);self.owner=Path(owner).resolve(strict=True)
        if self.product.binary.resolve(strict=True)!=self.binary:raise ValueError('Source owner public binary differs from product inputs')
        self.verify()

    def verify(self):
        m=self.manifest;self.product.verify()
        if self.path.is_symlink() or self.path.read_bytes()!=self.raw:raise ValueError('Source owner manifest changed')
        root=Path(m['source_root']).resolve(strict=True);build=Path(m['producer_build']).resolve(strict=True)
        home=re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$',(build/'CMakeCache.txt').read_text(encoding='utf-8'),re.MULTILINE)
        if not home or Path(home[1].strip()).resolve()!=root:raise ValueError('Compiler build belongs to another source root')
        version=json.loads(subprocess.check_output([str(self.binary),'--version','--json'],text=True,timeout=30))
        self.product.require_version(version)
        if (m['consumer_root']!=str(ROOT.resolve()) or m['code_head']!=git(root,'rev-parse','HEAD')
                or m['code_head']!=self.product.manifest['source_commit'] or git(root,'status','--porcelain')
                or git(ROOT,'status','--porcelain') or m['cli_version']!=version
                or m['cli_sha256']!=digest(self.binary.read_bytes())
                or m['cli_sha256']!=digest((build/'xray.exe').read_bytes())
                or m['product_inputs_manifest_sha256']!=digest(self.product.raw)
                or Path(m['cli_binary']).resolve()!=self.binary
                or Path(m['owner_binary']).resolve()!=self.owner
                or m['owner_sha256']!=digest(self.owner.read_bytes())
                or m['stdlib_path']!=str(self.product.stdlib)):
            raise ValueError('Source owner lost its exact fixed producer or consumer identity')
        if (set(m['consumer_inputs'])!=consumer_names()
                or m['consumer_inputs']!=file_table(ROOT,consumer_names())
                or m['producer_inputs']!=file_table(root,producer_names(root))
                or m['producer_build_inputs']!=file_table(build,BUILD_FILES)
                or m['link_inputs']!=link_inputs(build)):
            raise ValueError('Source owner compiler graph,archives,headers or consumer inputs changed')
        identities=compiler_identities(root);p=self.product.manifest['protocol']
        if m['compiler_identities']!=identities or identities!={'checked_schema':p['wire'],'semantic_identity':p['semantic'],'value_abi':p['value']}:
            raise ValueError('Source owner compiler protocol differs from its paired public product')
        faults=Path(m['fault_owner_binary']).resolve(strict=True)
        if faults.parent!=self.owner.parent or digest(faults.read_bytes())!=m['fault_owner_sha256']:
            raise ValueError('Source fault owner changed')
        task_rejection_model.expected()
        return m
