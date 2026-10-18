"""Derive storage packets from preserved complete independent75 field vectors."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from checked_storage_wire import receive_storage
from checked_scope_wire import receive_offset


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    directory=args.source_root/'tests/unit/xir'
    checked=(args.source_root/'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+30u',checked)
    assert re.search(r'XR_XIR_CHECKED_CONTRACT\s+76u',checked)
    records=[]
    previous=[]
    headers=[]
    for source in sorted(directory.glob('xir_*75_golden.h')):
        text=source.read_text(encoding='utf-8')
        positions=None
        digest=None
        headers.append(source.name)
        def row(match):
            nonlocal positions,digest
            name=match[1]
            data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            previous.append((name,len(data),hashlib.sha256(data).hexdigest()))
            if data[:8]==b'XRCHK\0\0\0':
                assert struct.unpack_from('<Q',data,24)[0]==len(data)-64
                assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]
                if struct.unpack_from('<II',data,8)==(29,75):
                    body,inserted=receive_storage(data[64:])
                    if positions is None: positions=inserted
                    else: assert positions==inserted
                    header=b'XRCHK\0\0\0'+struct.pack('<IIIIQ',30,76,2,0,len(body))
                    digest=hashlib.sha256(header+body).digest()
                    data=header+digest+body
                    records.append(dict(symbol=name.replace('75','76'),modules=len(inserted),
                                        bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
            elif name.endswith('75_digest'):
                assert digest is not None
                data=digest
            lines=['static const uint8_t '+name.replace('75','76')+'[] = {']
            lines.extend('    '+', '.join('0x%02x'%b for b in data[i:i+12])+','
                         for i in range(0,len(data),12))
            return '\n'.join(lines)+'\n};'
        text=re.sub(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',row,text,flags=re.S)
        def macro(match):
            name,value=match[1],int(match[2])
            value=receive_offset(value,positions or [])
            return '#define '+name.replace('75','76')+' '+str(value)+match[3]
        text=re.sub(r'#define (\w*75\w*) (\d+)(u?)\b',macro,text)
        text=text.replace('75_GOLDEN_H','76_GOLDEN_H').replace('75_golden.h','76_golden.h')
        target=source.with_name(source.name.replace('75','76'))
        if args.write: target.write_text(text,encoding='utf-8',newline='\n')
        else: assert target.read_text(encoding='utf-8')==text,target
    manifest=['/* Preserved independent packets and digest fragments, all rejected by the current reader. */',
              '#ifndef STORAGE76_PREVIOUS_VECTORS_H','#define STORAGE76_PREVIOUS_VECTORS_H']
    manifest+=['#include "'+h+'"' for h in headers]
    manifest+=['static const struct { const uint8_t *bytes; size_t length; } storage76_previous[] = {']
    manifest+=['    {'+n+',sizeof('+n+')},' for n,_,_ in previous]
    manifest+=['};','#endif // STORAGE76_PREVIOUS_VECTORS_H']
    text='\n'.join(manifest)+'\n'
    target=directory/'storage76_previous_vectors.h'
    if args.write: target.write_text(text,encoding='utf-8',newline='\n')
    else: assert target.read_text(encoding='utf-8')==text,target
    print(json.dumps(dict(records=records,preserved_exact_arrays=previous,product_writer_used=False),indent=2))


if __name__=='__main__':
    main()
