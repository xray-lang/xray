"""Verify independent i64/ASCII output and actual native/mixed owners."""
from pathlib import Path
import argparse,re,subprocess,sys
root=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(root/'scripts'))
from check_xr_program_aot_native import forbidden_symbol_family,load_symbol_inventory
MODULES={'typed_nominal': 2, 'typed_interface': 2, 'construction': 2, 'generic_parent_lr': 2, 'generic_parent_rl': 2, 'member_witness': 2, 'carrier_member': 2, 'enum_owned': 2, 'class_owned': 2}
EXPECTED_TEXT='41\n'
assert EXPECTED_TEXT.encode('ascii').hex()=='34310a'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable',type=Path,required=True);p.add_argument('--case',choices=MODULES,required=True)
    p.add_argument('--mode',choices=['native','vm','native-vm','vm-native'],required=True);a=p.parse_args()
    r=subprocess.run([str(a.executable.resolve()),'--census',a.case,a.mode],capture_output=True,timeout=110)
    sys.stdout.buffer.write(r.stdout);sys.stderr.buffer.write(r.stderr)
    assert r.returncode==0 and not r.stderr,(r.returncode,r.stderr)
    lines=r.stdout.decode('utf-8').splitlines();owners=[]
    for i in range(2):
        matches=[re.fullmatch(rf'TYPED_NATIVE_RESOURCE_OUTPUT file={a.case} instance={i} owner=(0x[0-9a-f]+) typed_groups=1 typed_type=(\d+) value=41 writes=1 bytes=34310a begins=1 ready=1 published=0 released=0 modules={MODULES[a.case]} begun_mask=(\d+) ready_mask=(\d+)',line) for line in lines]
        matches=[m for m in matches if m];assert len(matches)==1,lines
        owner,typ,begun,ready=matches[0].groups();assert int(owner,16) and int(typ)==2
        assert int(begun)==int(ready)==(1<<MODULES[a.case])-1;owners.append(owner)
        assert lines.count(f'TYPED_NATIVE_CALLS file={a.case} mode={a.mode} instance={i} resultCalls=2 entryCalls=1')==1
        owned=[re.fullmatch(rf'TYPED_NATIVE_RESOURCE_OWNED file={a.case} instance={i} owner={owner} held_type=2 held_value=41 alias_type=2 alias_value=41',line) for line in lines]
        assert sum(x is not None for x in owned)==1
        census=[line for line in lines if line.startswith(f'TYPED_NATIVE_RESOURCE_CENSUS file={a.case} instance={i} ')]
        assert len(census)==1 and census[0].endswith('compiler_physical=0/0 runtime_physical=0/0')
    assert owners[0]!=owners[1]
    providers=[re.fullmatch(rf'TYPED_NATIVE_RESOURCE_PROVIDER_INSTANCE mode={a.mode} witness=([01]) owner=(0x[0-9a-f]+) nativeSteps=(\d+) vmSteps=(\d+) nativeReleases=(\d+) vmReleases=(\d+) crossings=(\d+)',line) for line in lines]
    providers=[m for m in providers if m];assert len(providers)==2
    for m in providers:
        i,owner,ns,vs,nr,vr,c=m.groups();assert owner==owners[int(i)]
        if a.mode=='native':assert int(ns) and int(nr) and int(vs)==int(vr)==int(c)==0
        elif a.mode=='vm':assert int(vs) and int(vr) and int(ns)==int(nr)==int(c)==0
        else:assert int(ns) and int(nr) and int(vs) and int(vr) and int(c)
    assert lines.count(f'TYPED_NATIVE_RESOURCE_PROVIDERS mode={a.mode} fullWitnesses=2 codeReleases=1 physical=0/0')==1
    symbols,error=load_symbol_inventory(a.executable.resolve());assert symbols is not None,error
    assert forbidden_symbol_family(symbols) is None
    assert not re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_compile_(?:source_|emit_c)',symbols,re.I)
    if a.mode=='native':assert not re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_(?:vm_|compile_(?:specialize|vm_))',symbols,re.I)
    else:assert re.search(r'(?<![A-Za-z0-9_])_*xr_xir_compile_vm_bind(?![A-Za-z0-9_])',symbols)
    assert re.search(r'h1typed_[0-8]_f\d+',symbols)
    print('TYPED_NATIVE_INDEPENDENT actual i64=41 bytes=34310a twoInstances ownedAfterFree physical=0/0')
if __name__=='__main__':main()
