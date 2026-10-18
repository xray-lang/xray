"""Complete original filesystem consumers with independent byte/tree expectations.

This symbolic tree is a normal-operation acceptance model, not host I/O. It
retains writes, aliases, cwd restoration, all128 reopens and owned cleanup.
Metadata clocks and successful host operations remain explicit runtime duties.
The Windows plain-CRT read-only deletion projection is kept separately from
required successful cleanup; neither leaf is qualified by this Python model.
"""
from frozen_derivation_inputs import resolve as resolve_derivation
from pathlib import Path
import hashlib,posixpath,re
from independent_observation_format import render
ROOT=Path(__file__).resolve().parents[4]
_NAMES=('io_binary_file_boundary_direct','io_chmod_shared_core','io_path_result_shared_core',
 'io_read_dir_shared_core','io_remove_all_shared_core','io_system_direct','io_touch_shared_core','io_write_shared_core')
_PROGRAMS={n:'tests/diff/cases/semantics/stdlib/'+n+'.xr' for n in _NAMES}
_COUNTS=dict(zip(_NAMES,[10,8,14,15,10,61,10,13]))
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
 'spec/source/sections/014-13-built-in-functions.md','spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md','stdlib/io/io.xr','stdlib/path/path.xr','stdlib/os/os.xr',
 'stdlib/io/io.c','src/aot/xrt_io.h','src/shared/xr_io_core.h',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'src/aot/xrt_io.h': '263ee23032e10b34f4477b64f1b6c5c385b536b9fa5a1c2c7d704a2ad86e3a98',
 'src/shared/xr_io_core.h': '078d9c97c7682168d6e73bbf1a7159a9bf58aa38c3e43ffc35f911e30d223af1',
 'stdlib/io/io.c': '27bf025b5ceb0f7fa3281855d6238ae877e4cb1d0d05d50ea4c14a4d573671d1',
 'stdlib/io/io.xr': 'cbb670fe3a25e42a3fa96a3bf3460f9696f459c0fac059b82ba8bc99c4d17096',
 'stdlib/os/os.xr': '32733efee3a175b97087a6750d7c37447b58ede1aa0359d04877b81462331098',
 'stdlib/path/path.xr': 'c6d1721f27e93a385053105cfbf33d88dae8e038c79e29085b11dd70e08225a1',
 'tests/diff/cases/semantics/stdlib/io_binary_file_boundary_direct.xr': 'eb85afac2fea84e1efffb014e111bba870a5efeef3821fe3e6e8ba9d562c4965',
 'tests/diff/cases/semantics/stdlib/io_chmod_shared_core.xr': '20cfda4a5db9bfce27e70c45abddcba3e03e7348f56bed20307fa56e51c8d50d',
 'tests/diff/cases/semantics/stdlib/io_path_result_shared_core.xr': '13276f95c33f1ef1e3e2c5c8b96548efa46a7f0857d05502c09d7edb860f9d08',
 'tests/diff/cases/semantics/stdlib/io_read_dir_shared_core.xr': '3a974f9cf3dbb8e5459d1d3ea0dbdf867e774564f05e4789ba7ad51598f557ea',
 'tests/diff/cases/semantics/stdlib/io_remove_all_shared_core.xr': 'c40f2c73028132e2fc9a56c3f48ac489583b69e4b47e9b623add649affc425b9',
 'tests/diff/cases/semantics/stdlib/io_system_direct.xr': 'fbaf3939a10e4dda586e3209ec1a09f5cff7722e966cbc1b57390026d7ea9930',
 'tests/diff/cases/semantics/stdlib/io_touch_shared_core.xr': 'c77026ac0b940981808bddea29f05f9aad810701060f5e2b7e3f79c2c9e3f81f',
 'tests/diff/cases/semantics/stdlib/io_write_shared_core.xr': 'd24e3e6db981e1f72609d0484dd837925786dcd39b14411ccee3113d0a040126',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}


class Tree:
    """Flat namespace and immutable byte values; no real files or host paths."""
    def __init__(self,platform='windows',link_ok=False,touch_delta=0,delete_policy='required'):
        if platform not in ('windows','posix') or delete_policy not in ('required','plain_crt'):
            raise ValueError('Unreviewed provider model')
        self.platform=platform;self.link_ok=link_ok;self.touch_delta=touch_delta;self.delete_policy=delete_policy
        self.nodes={p:{'kind':'dir'} for p in ['/','/working','/owned']};self.cwd='/working'
        self.owned=[];self.events=[];self.directory_pairs=0

    def path(self,path):return posixpath.normpath(path if path.startswith('/') else self.cwd+'/'+path)
    def event(self,op,**fields):self.events.append({'operation':op,**fields})
    def exists(self,path):return self.path(path) in self.nodes
    def node(self,path):
        value=self.nodes.get(self.path(path))
        if value and value['kind']=='link':
            target=self.nodes.get(value['target'])
            if target and target['kind']=='link':raise ValueError('Nested symlink policy not modeled')
            return target
        return value
    def is_dir(self,path):return bool(self.node(path) and self.node(path)['kind']=='dir')
    def is_file(self,path):return bool(self.node(path) and self.node(path)['kind']=='file')
    def is_link(self,path):return bool(self.nodes.get(self.path(path),{}).get('kind')=='link')
    def temp_dir(self):
        name='/owned/dir'+str(len(self.owned)+1);self.nodes[name]={'kind':'dir'};self.owned.append(name)
        self.event('tempDir',path=name);return name
    def temp_file(self):
        name='/owned/file'+str(len(self.owned)+1);self.write(name,b'');self.owned.append(name)
        self.event('tempFile',path=name);return name
    def mkdirp(self,path):
        path=self.path(path);parts=path.strip('/').split('/')
        for i in range(len(parts)):
            key='/'+'/'.join(parts[:i+1])
            if key in self.nodes and self.nodes[key]['kind']!='dir':return False
            self.nodes[key]={'kind':'dir'}
        self.event('mkdirp',path=path);return True
    def write(self,path,data,append=False):
        path=self.path(path)
        if not self.is_dir(posixpath.dirname(path)) or self.is_dir(path):return False
        data=data.encode('utf-8') if isinstance(data,str) else bytes(data)
        old=self.node(path);previous=old['data'] if old and old['kind']=='file' else b''
        content=(previous if append else b'')+data
        self.nodes[path]={'kind':'file','data':content,'mode':420,'mtime':17}
        self.event('append' if append else 'write',path=path,data_hex=data.hex(),result_hex=content.hex());return True
    def read(self,path):
        node=self.node(path);return bytes(node['data']) if node and node['kind']=='file' else None
    def size(self,path):
        data=self.read(path);return len(data) if data is not None else -1
    def stat(self,path):
        node=self.node(path)
        if node is None:return None
        return {'size':len(node.get('data',b'')),'mode':node.get('mode',0),'mtime':node.get('mtime',17),
            'atime':17,'ctime':17,'uid':0,'gid':0,'isFile':node['kind']=='file','isDir':node['kind']=='dir','isSymlink':self.is_link(path)}
    def chmod(self,path,mode):
        node=self.node(path)
        if node is None or mode<0:return False
        node['mode']=mode;self.event('chmod',path=self.path(path),mode=mode);return True
    def touch(self,path):
        node=self.node(path)
        if node is None:ok=self.write(path,b'')
        else:node['mtime']=node.get('mtime',17)+self.touch_delta;ok=True
        self.event('touch',path=self.path(path));return ok
    def copy(self,source,target):
        data=self.read(source)
        if data is None:return False
        ok=self.write(target,data);self.event('copy',source=self.path(source),target=self.path(target));return ok
    def rename(self,source,target):
        source=self.path(source);target=self.path(target)
        if source not in self.nodes or not self.is_dir(posixpath.dirname(target)):return False
        self.nodes[target]=self.nodes.pop(source);self.event('rename',source=source,target=target);return True
    def read_dir(self,path):
        path=self.path(path)
        if not self.is_dir(path):return None
        self.directory_pairs+=1;prefix=path.rstrip('/')+'/'
        return sorted(k[len(prefix):] for k in self.nodes if k.startswith(prefix) and '/' not in k[len(prefix):])
    def recursive(self,path):
        path=self.path(path)
        if not self.is_dir(path):return None
        prefix=path.rstrip('/')+'/';subtree={k:v for k,v in self.nodes.items() if k==path or k.startswith(prefix)}
        self.directory_pairs+=sum(v['kind']=='dir' for v in subtree.values())
        return sorted(k[len(prefix):] for k in subtree if k!=path)
    def remove(self,path,tree=False):
        path=self.path(path)
        if path not in self.nodes:return False
        keys=[k for k in self.nodes if k==path or tree and k.startswith(path.rstrip('/')+'/')]
        if tree:self.directory_pairs+=sum(self.nodes[k]['kind']=='dir' for k in keys)
        removed=[]
        for key in sorted(keys,key=lambda k:(k.count('/'),k),reverse=True):
            node=self.nodes[key]
            if node['kind']=='file' and self.platform=='windows' and self.delete_policy=='plain_crt' and not node['mode']&128:continue
            if node['kind']=='dir' and any(k.startswith(key.rstrip('/')+'/') for k in self.nodes if k!=key):continue
            del self.nodes[key];removed.append(key)
        self.event('removeAll' if tree else 'remove',path=path,removed_paths=removed)
        return path not in self.nodes
    def realpath(self,path):
        key=self.path(path);value=self.nodes.get(key)
        return (value['target'] if value['kind']=='link' else key) if self.node(key) is not None else None
    def chdir(self,path):
        if not self.is_dir(path):return False
        self.cwd=self.path(path);self.event('chdir',path=self.cwd);return True
    def symlink(self,target,path):
        path=self.path(path)
        if not self.link_ok:self.event('symlink_refused',path=path);return False
        self.nodes[path]={'kind':'link','target':self.path(target)};self.event('symlink',path=path,target=self.path(target));return True
    def readlink(self,path):return self.nodes.get(self.path(path),{}).get('target')
    def live_owned(self):return sorted(k for k in self.nodes if any(k==p or k.startswith(p+'/') for p in self.owned))


def lines(data):
    if not data:return []
    pieces=data.split(b'\n');terminated=len(pieces)-1
    if not pieces[-1]:pieces.pop()
    return [(part[:-1] if i<terminated and part.endswith(b'\r') else part).decode('utf-8','replace') for i,part in enumerate(pieces)]


def polynomial_hash(data):
    # A closed polynomial, independent of the consumer's rolling loop.
    return sum(value*pow(131,len(data)-1-index,1000000007) for index,value in enumerate(data))%1000000007


def traces(platform='windows',link_ok=False,touch_delta=0,delete_policy='required'):
    result={}
    def fresh():return Tree(platform,link_ok,touch_delta,delete_policy)
    def finish(name,fs,values,**extra):
        if len(values)!=_COUNTS[name]:raise ValueError('Complete IO observation count changed: '+name)
        result[name]={'observations':values,'side_effects':fs.events,'live_owned_paths':fs.live_owned(),
            'cwd_restored':fs.cwd=='/working','directory_iterator_pair_obligations':fs.directory_pairs,**extra}
    f=fresh();root=f.temp_dir();file=root+'/all-bytes.bin';payload=bytes(range(256));v=[f.write(file,payload)];read=f.read(file)
    v.extend([read is not None,len(read),read[0],read[1],read[127],read[128],read[255],polynomial_hash(read)==290104970,f.remove(root,True)])
    finish('io_binary_file_boundary_direct',f,v,complete_payload_hex=payload.hex(),required_initialization_iterations=256,required_hash_loop_iterations=256,hash_value=polynomial_hash(payload))
    f=fresh();root=f.temp_dir();file=root+'/mode.txt';v=[bool(root),f.write(file,'mode'),f.chmod(file,420)]
    st=f.stat(file);v.append(st is not None if platform=='windows' else st is not None and st['mode']==420)
    v.append(f.chmod(file,0));st=f.stat(file);v.append(st is not None if platform=='windows' else st is not None and st['mode']==0)
    v.extend([f.remove(root,True),not f.exists(root)]);finish('io_chmod_shared_core',f,v,required_modes=[420,0])
    f=fresh();root=f.temp_dir();directory=root+'/paths';file=directory+'/target.txt';missing=directory+'/missing.txt'
    v=[bool(root),f.mkdirp(directory),f.write(file,'target'),f.realpath(missing) is None,f.readlink(missing) is None]
    resolved_dir=f.realpath(directory);resolved_file=f.realpath(file);v.extend([bool(resolved_dir),bool(resolved_file)])
    old=f.cwd;v.extend([f.chdir(directory),f.realpath('.')==resolved_dir,f.chdir(old)])
    link=root+'/target-link.txt';made=f.symlink(file,link)
    if made:
        target=f.readlink(link);v.extend([bool(target),f.realpath(target)==resolved_file])
    else:v.extend([True,True])
    v.extend([f.remove(root,True),not f.exists(root)]);finish('io_path_result_shared_core',f,v,symlink_branch=made)
    f=fresh();root=f.temp_dir();nested=root+'/a/b';v=[bool(root),f.mkdirp(nested),f.write(root+'/top.txt','top'),f.write(nested+'/deep.txt','deep')]
    entries=f.read_dir(root);v.extend(['top.txt' in entries,'a' in entries,'.' not in entries,'..' not in entries])
    reopened=[f.read_dir(root) is not None for attempt in range(128)];v.append(all(reopened));recursive=f.recursive(root)
    v.extend(name in recursive for name in ['top.txt','a','a/b','a/b/deep.txt']);missing=f.recursive(root+'/missing');v.append(0 if missing is None else len(missing))
    v.append(f.remove(root,True));finish('io_read_dir_shared_core',f,v,required_original_reopen_calls=128,reopen_results=reopened,entry_names=entries,recursive_names=recursive)
    f=fresh();root=f.temp_dir();nested=root+'/x/y';file=nested+'/a.txt';top=root+'/top.txt'
    v=[bool(root),f.mkdirp(nested),f.write(file,'a'),f.write(top,'b'),f.remove(file,True),not f.exists(file),f.write(file,'again'),f.remove(root,True),not f.exists(root),not f.remove(root,True)]
    finish('io_remove_all_shared_core',f,v)
    f=fresh();root=f.temp_dir();directory=root+'/a/b';v=[bool(root),f.mkdirp(directory),f.is_dir(directory),f.mkdirp(directory),f.mkdirp(directory+'///'),f.is_dir(directory)]
    file=directory+'/sample.txt';v.extend([f.write(file,'hello'),f.exists(file),f.is_file(file),not f.is_dir(file),f.size(file),f.read(file)==b'hello',f.write(file,'\nworld',True)])
    read_lines=lines(f.read(file));v.extend([len(read_lines),*read_lines])
    trailing=directory+'/trailing.txt';v.append(f.write(trailing,'alpha\n'));trailing_lines=lines(f.read(trailing));v.extend([len(trailing_lines),trailing_lines[0]])
    crlf=directory+'/crlf.txt';v.append(f.write(crlf,'a\r\n\r\nb\r\n'));crlf_lines=lines(f.read(crlf));v.extend([len(crlf_lines),crlf_lines[0],crlf_lines[1]=='',crlf_lines[2]])
    binary=directory+'/bytes.bin';v.append(f.write(binary,bytes([65,0,255])));data=f.read(binary);v.extend([len(data),*data])
    copied=root+'/copy.txt';moved=root+'/moved.txt';v.extend([f.copy(file,copied),f.read(copied)==b'hello\nworld',f.rename(copied,moved),not f.exists(copied),f.exists(moved)])
    st=f.stat(file);v.extend([st is not None,st['size']==11,*[st[k]>=0 for k in ['mode','mtime','atime','ctime','uid','gid']],st['isFile'],not st['isDir'],not st['isSymlink']])
    v.extend([f.touch(root+'/touched.txt'),f.chmod(file,420),len(f.read_dir(directory))>=2,len(f.recursive(root))>=4])
    resolved=f.realpath(directory);old=f.cwd;v.extend([bool(old),bool(resolved),f.chdir(directory),f.cwd==resolved,f.chdir(old)])
    link=root+'/sample-link.txt';made=f.symlink(file,link);v.extend([f.is_link(link),f.readlink(link) is not None] if made else [True,True])
    temp=f.temp_file();v.extend([bool(temp),f.exists(temp),f.remove(temp),f.remove(root,True),not f.exists(root)])
    finish('io_system_direct',f,v,symlink_branch=made,line_results=[read_lines,trailing_lines,crlf_lines],required_stat_predicates=['size==11','mode>=0','mtime>=0','atime>=0','ctime>=0','uid>=0','gid>=0','isFile','!isDir','!isSymlink'])
    f=fresh();root=f.temp_dir();file=root+'/touch.txt';v=[f.touch(file),f.exists(file),f.size(file),f.write(file,'payload')];before=f.stat(file);v.append(before is not None)
    v.append(f.touch(file));after=f.stat(file);v.extend([after is not None,f.read(file)==b'payload',after['mtime']>=before['mtime'],f.remove(root,True)])
    finish('io_touch_shared_core',f,v,required_mtime_predicate='after.mtime >= before.mtime',clock_delta_is_symbolic_not_host_measurement=True)
    f=fresh();root=f.temp_dir();text=root+'/write.txt';v=[f.write(text,''),f.size(text),f.write(text,'alpha',True),f.write(text,'',True),f.write(text,'\nomega',True),f.read(text)==b'alpha\nomega']
    binary=root+'/bytes.bin';v.append(f.write(binary,bytes([65,0,10,255])));data=f.read(binary);v.extend([len(data),*data,f.remove(root,True)])
    finish('io_write_shared_core',f,v)
    return result


def expected_rows():
    if set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or set(_PROGRAMS)!=set(_NAMES) or len(_PROGRAMS)!=8:
        raise ValueError('Complete filesystem source/authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, path, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Complete IO source or normative input changed: '+path)
    derived=traces();rows=[]
    for name in _NAMES:
        trace=derived[name]
        if trace['live_owned_paths'] or not trace['cwd_restored']:raise ValueError('Original successful cleanup/cwd obligations lost')
        row={'id':'complete_filesystem_io_'+name,'source':_PROGRAMS[name],'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':x})+'\n' for x in trace['observations']),'stderr_utf8':''},
            'observation_count':len(trace['observations']),'required_semantic_trace':{k:v for k,v in trace.items() if k!='observations'},
            'normal_environment_requirements':['Original fresh temporary paths and all successful host operations','Original nonnegative stat metadata and nondecreasing observed touch clock'],
            'model':'Complete original filesystem body;independent symbolic byte/tree/line/hash/cwd/cleanup expectations,all128reopens retained. Symbolic metadata is not host measurement;Windows plain-CRT readonly deletion conflict remains open. No actual I/O/descriptor release/permissions/clock/atomicity/FI/resource/lifetime/provider qualification.'}
        if name=='io_chmod_shared_core':row['current_windows_leaf_projection']={
            'required_observations':trace['observations'],
            'plain_crt_observations':traces(delete_policy='plain_crt')[name]['observations'],
            'VM_source_clears_readonly':True,'legacy_AOT_source_clears_readonly':False,
            'qualification':'SOURCE_AND_HOST_OPERATION_ONLY;current_generated_native_route_not_proved'}
        rows.append(row)
    return rows + [dylib_expected_row()] + system_query_expected_rows()

_DYLIB_SOURCE = 'tests/diff/cases/semantics/stdlib/sys_dylib_direct.xr'
_DYLIB_ID = 'complete_filesystem_io_sys_dylib_direct'
_DYLIB_INPUTS = {'tests/diff/cases/semantics/stdlib/sys_dylib_direct.xr': '6d3d4c872de3993d70ee8fc113c76d1616f71879ee37335dcfdd1ee7e679aa27', 'stdlib/sys/sys.xr': '8e508319d317801403b633b8f27c7fb6154be5f0743ea2bdb6f1fca52bba3471', 'stdlib/os/os.xr': '32733efee3a175b97087a6750d7c37447b58ede1aa0359d04877b81462331098', 'stdlib/path/path.xr': 'c6d1721f27e93a385053105cfbf33d88dae8e038c79e29085b11dd70e08225a1', 'stdlib/mem/mem.xr': '9750e120d6b22f5c2caffd2d59d13238393ac6939c17827090a3829c33fa9dab', 'src/coro/xsys_provider.c': '2d05ef5bed8783936782127ccf59b480b9321eb86d94d2cc0e0b04cef77c3410', 'src/os/win/dylib_win.c': '4c950d9ebe81dd5d86df9fca4b01827694879c83301786af3fe5b30cd8e212a6', 'src/aot/xrt_sys.h': '40edd10a2a61514a031a795797622b254ff88e1f195003397c588f017252f24e'}

class DylibSymbols:
    """Explicit normal host facts; symbolic addresses never call foreign code."""
    def __init__(self, available=True, exported=True, missing_export=False,
                 missing_library=False, error_text='host error', close_ok=True):
        self.available=available;self.exported=exported
        self.missing_export=missing_export;self.missing_library=missing_library
        self.error_text=error_text;self.close_ok=close_ok
        self.last_error='';self.calls=[]

    def open(self, name):
        self.calls.append(('open',name))
        found=self.available if name=='kernel32.dll' else self.missing_library
        self.last_error='' if found else self.error_text
        return 1 if found else 0

    def symbol(self, handle, name):
        self.calls.append(('symbol',handle,name))
        found=self.exported if name=='GetCurrentProcessId' else self.missing_export
        self.last_error='' if found else self.error_text
        return 17 if found else None

    def close(self, handle):
        self.calls.append(('close',handle))
        return self.close_ok


class DylibHandle:
    """Public close is idempotently true; a closed symbol avoids the provider."""
    def __init__(self, provider, handle):
        self.provider=provider;self.handle=handle;self.is_open=handle!=0

    def symbol(self, name):
        if not self.is_open:return None
        return self.provider.symbol(self.handle,name)

    def close(self):
        if not self.is_open:return True
        ok=self.provider.close(self.handle)
        self.handle=0;self.is_open=False
        return ok


def dylib_handle_trace(provider=None):
    if provider is None:provider=DylibSymbols()
    handle=provider.open('kernel32.dll')
    lib=DylibHandle(provider,handle) if handle else None
    out=[lib is not None]
    if lib is not None:
        symbol=lib.symbol('GetCurrentProcessId')
        out.append(symbol is not None)
        if symbol is not None:out.append(symbol!=0)
        missing=lib.symbol('__xray_missing_symbol_for_dylib_test__')
        out.extend([missing is None,len(provider.last_error)>0])
        out.extend([lib.close(),lib.close(),lib.symbol('GetCurrentProcessId') is None])
    missing_lib=provider.open('__xray_missing_library_for_dylib_test__')
    out.extend([missing_lib==0,len(provider.last_error)>0])
    return {'observations':out,'provider_calls':provider.calls,
            'logical_handle':None if lib is None else lib.handle,
            'logical_open':False if lib is None else lib.is_open,
            'original_print_expressions':10,
            'host_preconditions':['Windows x64','kernel32.dll opens',
                'GetCurrentProcessId has nonzero address','both reserved missing names are absent',
                'normal first FreeLibrary succeeds','failure diagnostics are nonempty'],
            'function_address_invoked':False,'host_provider_orphysical_release_qualified':False}


def dylib_expected_row():
    expected={_DYLIB_SOURCE,'stdlib/sys/sys.xr','stdlib/os/os.xr','stdlib/path/path.xr',
              'stdlib/mem/mem.xr','src/coro/xsys_provider.c','src/os/win/dylib_win.c','src/aot/xrt_sys.h'}
    if set(_DYLIB_INPUTS)!=expected:raise ValueError('Dynamic library complete inputs changed')
    for name,digest in _DYLIB_INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Dynamic library original or reviewed authority changed: '+name)
    trace=dylib_handle_trace()
    return {'id':_DYLIB_ID,'source':_DYLIB_SOURCE,'original_source':_DYLIB_SOURCE,
            'required_target':'windows-x86_64','expected':{'exit_code':0,'stderr_utf8':'',
                'stdout_utf8':''.join(render({'expected':value})+'\n' for value in trace['observations'])},
            'model':'Complete unchanged Windows dynamic-library original; all five imports and ten print expressions retained. Symbolic handle and symbol availability, missing-name errors, first close and idempotent true second close, closed-symbol null. Host normal-operation preconditions remain runtime duties; no foreign function invocation, failure injection, physical release, lifetime, safety or provider qualification.',
            'observation_count':len(trace['observations']),'required_semantic_trace':trace}

_SYSTEM_PROGRAMS = {'os_query_system_direct': 'tests/diff/cases/semantics/stdlib/os_query_system_direct.xr', 'datetime_offset_system_direct': 'tests/diff/cases/semantics/stdlib/datetime_offset_system_direct.xr'}
_SYSTEM_QUERY_IDS = frozenset('complete_filesystem_io_'+name for name in _SYSTEM_PROGRAMS)
_SYSTEM_QUERY_INPUTS = {'tests/diff/cases/semantics/stdlib/os_query_system_direct.xr': '17b634ce318e4f5f1300590cbb3da392d8120ac4c02c3dcbb6ed2b2748dba565', 'tests/diff/cases/semantics/stdlib/datetime_offset_system_direct.xr': '3c101428c24c856ebd125816094102692a1ee850f22edc349e9e7c9fe8450581', 'stdlib/os/os.xr': '32733efee3a175b97087a6750d7c37447b58ede1aa0359d04877b81462331098', 'stdlib/io/io.xr': 'cbb670fe3a25e42a3fa96a3bf3460f9696f459c0fac059b82ba8bc99c4d17096', 'stdlib/path/path.xr': 'c6d1721f27e93a385053105cfbf33d88dae8e038c79e29085b11dd70e08225a1', 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72', 'stdlib/datetime/datetime.xr': 'cd05dfebf4afb1d5a1bf2326aa84afd2dbe17825d90ca6f9a340751db596197d', 'stdlib/os/os.c': '0ac1a21b27d996c97268ebeb0bd41205e5687473feda760d7f7b88485fbb2996', 'stdlib/time/time.c': '71bae1e19f5f0d74fac3065562f90c5ea159ef8505d2b2b8e4101f56cc7b8e42', 'src/aot/xrt_os.h': 'c28f5d0067589be7d7df7d07e06030d9a81b2757524175a52937a6938c0ca2b6', 'src/aot/xrt_time.h': 'f4e38e81e6bb62b0286e722cdd9b4a334c0587ca968fcb0f9bebb4a05c5c01ac', 'src/shared/xr_os_core.h': 'd453e8f88f12d48030e115ef7de091ee73f79f956635e049839f32fc7045b9ca', 'src/shared/xr_time_offset.h': 'df5ee669531c5c252d2604f8cfea1c433148feb59070822eb70f1ec04eb7ff1c'}

class SystemEnvironment:
    """Keep absent and empty bindings distinct; no process environment changes."""
    def __init__(self, initial=None, empty_policy='preserve'):
        if empty_policy not in ('preserve','crt_remove'):raise ValueError('Unreviewed empty environment policy')
        self.bindings=dict(initial or {});self.events=[];self.empty_policy=empty_policy

    def get(self, name):
        self.events.append(('getenv',name))
        return self.bindings.get(name)

    def set(self, name, value):
        self.events.append(('setenv',name,value))
        if self.empty_policy=='crt_remove' and value=='':self.bindings.pop(name,None)
        else:self.bindings[name]=value
        return True

    def unset(self, name):
        self.events.append(('unsetenv',name));self.bindings.pop(name,None);return True

    def environ(self):
        self.events.append(('environ',))
        return dict(self.bindings)

    def restore(self, name, value):
        if value is None:self.unset(name)
        else:self.set(name,value)

    def tmpdir(self):
        for name in ('TMPDIR','TMP','TEMP'):
            value=self.get(name)
            if value is not None and len(value)>0:return value
        return 'C:\\Windows\\Temp'

    def homedir(self, system):
        for name in ('HOME','USERPROFILE'):
            value=self.get(name)
            if value is not None and len(value)>0:return value
        return system if system is not None and len(system)>0 else None


def system_query_trace(initial=None, facts=None, environment_class=SystemEnvironment):
    required={'platform':'windows','arch':'x64','sep':'\\','eol':'\r\n','pid':1,'ppid':2,
              'uid':0,'gid':0,'cpu_count':1,'signal_zero_ok':True,'total_memory':16,'free_memory':8,
              'uptime':0.0,'loadavg':[0.0,0.0,0.0],'clock':0,'cwd':'/working',
              'chdir_ok':True,'hostname':'host','username':'user','system_home':'/profile'}
    required.update(facts or {});f=required;environment=environment_class(initial)
    before=dict(environment.bindings)
    out=[len(f['platform'])>0,len(f['arch'])>0,len(f['sep'])>0,len(f['eol'])>=1,
         f['pid']>0,f['uid']>=0,f['gid']>=0,f['cpu_count']>=1,f['ppid']>0,
         f['signal_zero_ok'],f['total_memory']>0,f['free_memory']>0,f['uptime']>=0.0,
         len(f['loadavg'])==3,*[f['loadavg'][i]>=0.0 for i in range(3)],f['clock']>=0]
    out.append(environment.get('XRAY_TEST_NONEXISTENT_VAR_12345') is None)
    out.extend([environment.set('XRAY_AOT_OS_QUERY_TMP','hello-aot'),
                environment.get('XRAY_AOT_OS_QUERY_TMP')=='hello-aot',
                environment.unset('XRAY_AOT_OS_QUERY_TMP'),
                environment.get('XRAY_AOT_OS_QUERY_TMP') is None])
    out.append(environment.set('XRAY_AOT_OS_QUERY_ENV','map-aot'))
    populated=environment.environ();out.append(populated['XRAY_AOT_OS_QUERY_ENV']=='map-aot')
    out.append(environment.unset('XRAY_AOT_OS_QUERY_ENV'))
    out.append(environment.set('XRAY_AOT_OS_QUERY_EMPTY',''))
    empty=environment.environ();out.append(empty['XRAY_AOT_OS_QUERY_EMPTY']=='')
    out.append(environment.unset('XRAY_AOT_OS_QUERY_EMPTY'))
    cwd=f['cwd'];out.extend([len(cwd)>0,f['chdir_ok'],f['cwd']==cwd,len(f['hostname'])>0,len(environment.tmpdir())>0])
    snapshots={name:environment.get(name) for name in ('TMPDIR','TMP','TEMP')}
    environment.set('TMPDIR','')
    environment.set('TMP','/tmp/xray_diff_tmpdir_fallback')
    environment.set('TEMP','/tmp/xray_diff_tmpdir_wrong')
    out.append(environment.tmpdir()=='/tmp/xray_diff_tmpdir_fallback')
    for name,value in snapshots.items():environment.restore(name,value)
    out.extend([f['username'] is not None,environment.homedir(f['system_home']) is not None])
    old_home=environment.get('HOME');environment.set('HOME','')
    fallback_home=environment.homedir(f['system_home'])
    out.extend([fallback_home is not None,len(fallback_home or '')>0])
    environment.restore('HOME',old_home)
    owned=['XRAY_AOT_OS_QUERY_TMP','XRAY_AOT_OS_QUERY_ENV','XRAY_AOT_OS_QUERY_EMPTY']
    return {'observations':out,'environment_before':before,'environment_after':dict(environment.bindings),
            'events':environment.events,'map_snapshot_nonempty':populated['XRAY_AOT_OS_QUERY_ENV'],
            'map_snapshot_empty':empty['XRAY_AOT_OS_QUERY_EMPTY'],
            'saved_fallback_bindings':dict(snapshots,HOME=old_home),
            'fallback_bindings_restored':all(environment.bindings.get(name)==value and
                (name in environment.bindings)==(value is not None) for name,value in dict(snapshots,HOME=old_home).items()),
            'owned_test_bindings_remaining':[name for name in owned if name in environment.bindings],
            'cwd_change_target':cwd,'cwd_after':f['cwd'],'clock_sample_count':1,
            'original_print_expressions':39,'host_provider_IO_resource_lifetime_qualified':False,
            'normal_host_requirements':['All original OS predicates succeed',
                'Reserved nonexistent name and three owned test names start absent',
                'All original set/unset/chdir operations succeed;empty strings remain present bindings',
                'Nonempty host username and home fallback exist']}


def timezone_offset_trace(first=0, second=0):
    # The second original call observes only its lower bound, not equality.
    return {'observations':[first>=-1440,first<=1440,second>=-1440],
            'original_offset_call_count':2,'first_read_used_twice':True,
            'requires_same_timestamp_or_offset':False,'exact_offset_value_asserted':False,
            'timezone_provider_orclock_failure_qualified':False}


def system_query_expected_rows():
    programs={name:'tests/diff/cases/semantics/stdlib/'+name+'.xr'
              for name in ('os_query_system_direct','datetime_offset_system_direct')}
    authorities={'stdlib/os/os.xr','stdlib/io/io.xr','stdlib/path/path.xr','stdlib/time/time.xr',
                 'stdlib/datetime/datetime.xr','stdlib/os/os.c','stdlib/time/time.c',
                 'src/aot/xrt_os.h','src/aot/xrt_time.h','src/shared/xr_os_core.h','src/shared/xr_time_offset.h'}
    if _SYSTEM_PROGRAMS!=programs or set(_SYSTEM_QUERY_INPUTS)!=set(programs.values())|authorities:
        raise ValueError('Whole system query identities or required authorities changed')
    if _SYSTEM_QUERY_IDS!=frozenset('complete_filesystem_io_'+name for name in programs):
        raise ValueError('System query product identities changed')
    for name,digest in _SYSTEM_QUERY_INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Whole system query source or authority changed: '+name)
    traces={'os_query_system_direct':system_query_trace(),'datetime_offset_system_direct':timezone_offset_trace()}
    rows=[]
    for name,source in sorted(programs.items()):
        trace=traces[name]
        rows.append({'id':'complete_filesystem_io_'+name,'source':source,'original_source':source,
                     'required_target':'windows-x86_64','expected':{'exit_code':0,'stderr_utf8':'',
                         'stdout_utf8':''.join(render({'expected':value})+'\n' for value in trace['observations'])},
                     'observation_count':len(trace['observations']),'required_semantic_trace':trace,
                     'model':('Complete unchanged OS query original,all four imports and39prints. Independent symbolic environment distinguishes absent/empty,keeps both map snapshots,all owned writes/unsets,cwd observation,TMPDIR/TMP/TEMP andHOME restoration. All system predicates remain normal host prerequisites;Windows CRT empty-binding projection remains OPEN,no actual I/O/clock/metadata/provider/FI/lifetime qualification.' if name=='os_query_system_direct' else
                         'Complete unchanged timezone-offset original andtwo calls;first sample lower/upper bounds,second sample onlylower bound. No exact offset,+480,currenttimestamp/equality requirement,hostclock/timezone/provider/failure/lifetime qualification.')})
    return rows
