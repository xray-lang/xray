"""Inspect actual COFF archive definitions; old compiler objects must not bind."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--archive',type=Path,required=True)
p.add_argument('--header',type=Path,required=True)
p.add_argument('--observe',action='store_true')
a=p.parse_args()
raw=a.archive.read_bytes()
assert raw[:8]==b'!<arch>\n' and len(raw)>=68
member=raw[8:68];assert member[58:60]==b'`\n'
name=member[:16].strip();size=int(member[48:58].strip())
payload=raw[68:68+size];assert len(payload)==size
assert name in (b'/',b'/SYM64/'),name
width=8 if name==b'/SYM64/' else 4
count=struct.unpack('>Q' if width==8 else '>I',payload[:width])[0]
begin=width*(count+1);assert begin<=size
names=payload[begin:].split(b'\0');assert len(names)>=count
symbols={s.decode('ascii') for s in names[:count]}
old='xr_xir_compile_construction_new';new=old+'_v4'
retired=any(s in symbols or '_'+s in symbols for s in [old,old+'_v2',old+'_v3'])
present=new in symbols or '_'+new in symbols
header=a.header.read_text('utf-8')
version=bool(re.search(r'#define\s+XR_XIR_CONSTRUCTION_C_INTERFACE_VERSION\s+4u\b',header))
probes=[]
if not a.observe:
    assert present and not retired,(present,retired)
    assert version and re.search(r'\b'+new+r'\s*\(',header)
    assert not re.search(r'\b'+old+r'\s*\(',header)
    for name in (old+'_v2',old+'_v3'):
        assert not re.search(r'\b'+name+r'\s*\(',header)
    cache=next(parent/'CMakeCache.txt' for parent in a.archive.resolve().parents
               if (parent/'CMakeCache.txt').is_file())
    compiler=re.search(r'^CMAKE_C_COMPILER:[^=]+=(.+)$',cache.read_text(encoding='utf-8'),re.M)[1].strip()
    common='#include "xir/xxir_construction.h"\n#include <stddef.h>\n'
    positive='''typedef XrXirStatus (*Constructor)(const XrXirCompileContext *,
        const XrXirTypes *, const XrXirConstructionSpec *, const XrXirSlotFacts *, XrXirConstruction **);
_Static_assert(_Generic(&xr_xir_compile_construction_new_v4,Constructor:1,default:0),"complete_constructor");
typedef const XrXirSlotFacts *(*Slots)(const XrXirConstruction *);
_Static_assert(_Generic(&xr_xir_compile_construction_slots,Slots:1,default:0),"borrowed_complete_slots");
_Static_assert(sizeof(XrXirSlotFact)==24 && _Alignof(XrXirSlotFact)==8,"slot_row_layout");
_Static_assert(offsetof(XrXirSlotFact,slot)==0 && offsetof(XrXirSlotFact,name)==8 &&
    offsetof(XrXirSlotFact,name_length)==16 && offsetof(XrXirSlotFact,exported)==20,"slot_fields");
_Static_assert(sizeof(XrXirSlotFacts)==16 && offsetof(XrXirSlotFacts,count)==8,"slot_view_layout");
'''
    inputs=[('current',positive,True,'')]
    for name in (old,old+'_v2',old+'_v3'):
        inputs.append((name,'void *retired_address(void) { return (void *)&'+name+'; }\n',False,name))
    inputs.append(('old_arity','XrXirStatus wrong_arity(void) { XrXirConstruction *out=0; return '+new+'(0,0,0,&out); }\n',False,new))
    inputs.append(('old_prototype','''typedef XrXirStatus (*Retired)(const XrXirCompileContext *,
    const XrXirTypes *,const XrXirConstructionRow *,uint32_t,XrXirConstruction **);
_Static_assert(_Generic(&xr_xir_compile_construction_new_v4,Retired:1,default:0),"retired_constructor_prototype");
''',False,'retired_constructor_prototype'))
    with tempfile.TemporaryDirectory(prefix='xir-construction-interface-') as temporary:
        directory=Path(temporary)
        for name,body,expected,diagnostic in inputs:
            source=directory/(name+'.c');source.write_text(common+body,encoding='utf-8')
            command=[compiler,'/nologo','/std:c11','/utf-8','/W4','/WX',
                     '/D_CRT_SECURE_NO_WARNINGS','/D_CRT_NONSTDC_NO_WARNINGS','/I'+str(a.header.resolve().parent.parent),
                     '/I'+str(a.header.resolve().parent.parent.parent/'include'),'/c',str(source),'/Fo'+str(directory/(name+'.obj'))]
            run=subprocess.run(command,cwd=directory,capture_output=True)
            output=(run.stdout+run.stderr).decode('utf-8',errors='replace')
            assert (run.returncode==0)==expected,(name,run.returncode,output)
            if not expected:assert diagnostic in output,(name,output)
            probes.append(dict(name=name,command=command,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                               expected_compile=expected,exit=run.returncode,output=output))
print(json.dumps(dict(archive=str(a.archive),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),
                     public_symbols=count,old_unversioned_symbol_present=retired,
                     new_versioned_symbol_present=present,compiler_interface_version4=version,compiler_probes=probes,
                     qualification='Actual archive definitions; no product execution or compatibility alias'),indent=2))
