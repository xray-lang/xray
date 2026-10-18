"""Inspect actual COFF archive definitions; old compiler objects must not bind."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

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
old='xr_xir_compile_construction_new';new=old+'_v3'
retired=any(s in symbols or '_'+s in symbols for s in [old,old+'_v2'])
present=new in symbols or '_'+new in symbols
header=a.header.read_text('utf-8')
version=bool(re.search(r'#define\s+XR_XIR_CONSTRUCTION_C_INTERFACE_VERSION\s+3u\b',header))
if not a.observe:
    assert present and not retired,(present,retired)
    assert version and re.search(r'\b'+new+r'\s*\(',header)
    assert not re.search(r'\b'+old+r'\s*\(',header)
print(json.dumps(dict(archive=str(a.archive),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),
                     public_symbols=count,old_unversioned_symbol_present=retired,
                     new_versioned_symbol_present=present,compiler_interface_version3=version,
                     qualification='Actual archive definitions; no product execution or compatibility alias'),indent=2))
