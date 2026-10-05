"""Independent framing/PE oracle plus one real native executable consumer."""
from pathlib import Path
import hashlib, json, os, shutil, struct, subprocess, sys, tempfile, time
exe, root, compiler, linker, sdk = map(lambda x: Path(x).resolve(), sys.argv[1:6])
mode=sys.argv[6]
def u(n,w=4): return int(n).to_bytes(w,'little')
def text(s):
    b=s.encode('utf-8');return u(len(b))+b
def digest(b): return hashlib.sha256(b).digest()
def verify(facts, binary):
    d=json.loads(facts.read_text(encoding='utf-8')); b=binary.read_bytes()
    pe=struct.unpack_from('<I',b,60)[0];opt=pe+24
    assert b[:2]==b'MZ' and b[pe:pe+4]==b'PE\0\0'
    machine,sections=struct.unpack_from('<HH',b,pe+4);optional,flags=struct.unpack_from('<HH',b,pe+20)
    magic=struct.unpack_from('<H',b,opt)[0];entry=struct.unpack_from('<I',b,opt+16)[0]
    image,headers=struct.unpack_from('<II',b,opt+56);subsystem=struct.unpack_from('<H',b,opt+68)[0]
    found=[]
    for i in range(sections):
        at=opt+optional+40*i;virtual,va,size,raw=struct.unpack_from('<IIII',b,at+8)
        if va<=entry<va+max(virtual,size):
            assert entry-va<size and raw+entry-va<len(b)
            found.append(struct.unpack_from('<I',b,at+36)[0])
    assert len(found)==1 and machine==0x8664 and magic==0x20b and flags&2 and not flags&0x2000 and subsystem==3 and found[0]&0x20000000
    image_frame=struct.pack('<HHHHIIII',machine,magic,flags,subsystem,entry,image,headers,found[0])
    frame=b'xray:xir-native-commands:trusted-local:v2'+u(2)+u(3)
    for stage,c in enumerate(d['commands']):
        frame+=u(stage)+text(c['executable'])+text(c['cwd'])+u(len(c['argv']))+b''.join(text(x) for x in c['argv'])
        frame+=u(len(c['env']))+b''.join(text(k)+text(v) for k,v in c['env'])+u(c['timeout'])+u(c['limit'],8)+u(c['image'])+u(c['completion'])
    commands=digest(frame)
    rows=sorted((r for r in d['files'] if r[1] in (3,4,7,8,9)),key=lambda r:(r[0],r[1],r[2].encode('utf-8')))
    frame=b'xray:xir-native-observed-inputs:trusted-local:v1'+u(1)+u(len(rows))
    frame+=b''.join(u(s)+u(k)+text(p)+u(n,8)+bytes.fromhex(h) for s,k,p,n,h in rows)
    sysroot=digest(frame); words=d['input_words'];sdk_id=bytes.fromhex(d['sdk_id']);triple='x86_64-pc-windows-msvc'
    frame=b'xray:xir-native-target-profile:trusted-local:v1'+u(1)+u(1)+u(3)+text(triple)+u(words[6])+u(words[3])
    frame+=b''.join(u(n) for n in words[1:6])+b''.join(u(n) for n in d['sdk'])+image_frame
    for p in (d['compiler'],d['linker']):
        frame+=text(p['path'])+u(p['length'],8)+bytes.fromhex(p['digest'])+b''.join(u(n,2) for n in p['version'])
    profile=digest(frame+commands+sysroot+sdk_id)
    ids=[digest(b'xray:toolchain:provider-version:v2'+text(d['compiler']['file_text'])),
         digest(b'xray:toolchain:target-triple:v2'+text(triple)),
         digest(b'xray:toolchain:codegen-options:v2'+text('xray:xir-native-commands:trusted-local:v2:sha256:'+commands.hex())),
         sysroot,sdk_id,profile]
    binding=digest(b'xray:aot-toolchain:v2'+u(2)+u(3)+b''.join(ids))
    h=list(map(bytes.fromhex,d['hashes']))
    assert h[5:11]==ids and h[11]==binding
    native_input=digest(b'xray:xir-native-input:v2'+b''.join(u(n) for n in words)+b''.join(h[:5])+binding)
    native=digest(b)
    artifact=digest(b'xray:xir-native-artifact:v2'+u(2)+u(len(b),8)+native_input+native)
    assert h[12:]==[native_input,native,artifact]
    print('independent PE/commands/sysroot/profile/Binding2/Input2/Artifact2 oracle PASS',flush=True)
with tempfile.TemporaryDirectory(prefix='xray-native-admission-') as temporary:
    parent=Path(temporary); marker=parent/'unrelated.txt';marker.write_bytes(b'unchanged')
    exported=parent/'export.exe';facts=parent/'facts.json'
    if mode=='unit': command=[exe,'unit',parent/'fixture.exe',exported,facts]
    else:
        authority=root/'tests/fixtures/xir_compile_owner';source=authority/'root.xr'
        vc=Path(os.environ['VCToolsInstallDir']);kit=Path(os.environ['WindowsSdkDir']);version=os.environ['WindowsSDKVersion'].strip('/\\')
        libraries=[vc/'lib/x64'/name for name in ('msvcrt.lib','oldnames.lib','vcruntime.lib')]
        libraries += [kit/'Lib'/version/'ucrt/x64/ucrt.lib',kit/'Lib'/version/'um/x64/kernel32.Lib']
        command=[exe,'native',authority,source,root/'stdlib',sdk,compiler,linker,parent,*libraries,vc/'include',
            kit/'Include'/version/'ucrt',kit/'Include'/version/'shared',kit/'Include'/version/'um',os.environ['SystemRoot'],exported,facts]
        for path in [source,authority/'counter.xr',authority/'label.xr',sdk/'sdk_manifest.json',sdk/'src/execution/xr_xir_native_main.inc.c']:
            print('input',path,'sha256='+hashlib.sha256(path.read_bytes()).hexdigest(),flush=True)
    print('argv='+json.dumps(list(map(str,command)),ensure_ascii=False),flush=True)
    started=time.perf_counter();result=subprocess.run(list(map(str,command)),capture_output=True,timeout=280)
    sys.stdout.buffer.write(result.stdout);sys.stderr.buffer.write(result.stderr)
    print('case',mode,'return',result.returncode,'seconds',round(time.perf_counter()-started,3),flush=True)
    assert result.returncode==0
    if os.environ.get('ADMISSION_EVIDENCE'):
        evidence=Path(os.environ['ADMISSION_EVIDENCE']);evidence.mkdir(parents=True,exist_ok=True)
        shutil.copy2(facts,evidence/(mode+'-facts.json'));shutil.copy2(exported,evidence/(mode+'-bytes.exe'))
    verify(facts,exported)
    assert marker.read_bytes()==b'unchanged'
    assert not any(p.name.startswith('xray-') for p in parent.iterdir())
    if mode=='native':
        result=subprocess.run([str(exported)],capture_output=True,timeout=30)
        assert (result.returncode,result.stdout,result.stderr)==(0,b'owner\xe4\xb8\xad-ok 42 true\n',b''),result
        print('actual three-module SDK executable independent LF output PASS',flush=True)