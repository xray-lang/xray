"""Independently frame current group packets and preserve the complete prior model."""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import derive_slot_group73_golden as roles

ROOT=Path(__file__).resolve().parent

def packet(unit):
    init=[roles.instruction(roles.SLOT_GROUP_INIT,immediate=3),roles.instruction(roles.RETURN)] if unit else [
        roles.instruction(roles.CONST_INT,roles.I64,immediate=1),
        roles.instruction(roles.CONST_INT,roles.I64,immediate=2),
        roles.instruction(roles.TUPLE_NEW,roles.TUPLE,0,2),
        roles.instruction(roles.CONST_INT,roles.I64,immediate=3),
        roles.instruction(roles.CONST_INT,roles.I64,immediate=4),
        roles.instruction(roles.TUPLE_NEW,roles.TUPLE,2,2),
        roles.instruction(roles.SLOT_GROUP_INIT,left=4,right=2,immediate=3),roles.instruction(roles.RETURN)]
    body=roles.words(0,2,1)
    body+=roles.function('init',roles.UNIT,init,() if unit else (0,1,3,4,2,5))
    body+=roles.function('main',roles.I64,[roles.instruction(roles.CONST_INT,roles.I64,immediate=41),roles.instruction(roles.RETURN)])
    body+=roles.words(1,3,0,0,1)
    # Root has no dependencies, initializer zero, lexical scope zero and fresh storage.
    body+=roles.words(4)+b'root'+roles.words(0,0,0,0)
    body+=roles.words(0,0,0,0,0,0,0,0,0)
    body+=roles.words(0,1,0,0,0,0,0,0,0)
    body+=roles.words(0,roles.UNIT if unit else roles.TUPLE,0,0,roles.UNIT,0,0,roles.UNIT if unit else roles.TUPLE,0)
    body+=roles.words(0,0)
    body+=roles.words(1,0,0,6,0,2,roles.I64,roles.I64)
    body+=roles.words(0,0,0)
    header=b'XRCHK\0\0\0'+roles.words(30,76,2,0)+struct.pack('<Q',len(body))
    return header+hashlib.sha256(header+body).digest()+body

def derive():
    legacy,_,_=roles.derive()
    assert (ROOT/'slot_group73_golden.h').read_text(encoding='utf8')==legacy,'Complete prior73 model changed'
    lines=['/* Independent wire30 semantic76 group packets with explicit scope and storage. */']
    rows=[]
    for name,unit in [('slot_group_mixed76_golden',False),('slot_group_unit76_golden',True)]:
        raw=packet(unit);lines.append('static const uint8_t '+name+'[] = {')
        lines.extend('    '+','.join(f'0x{byte:02x}' for byte in raw[index:index+12])+',' for index in range(0,len(raw),12))
        lines.append('};');rows.append({'name':name,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
            'body_sha256':hashlib.sha256(raw[64:]).hexdigest()})
    return '\n'.join(lines)+'\n',rows

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--write',action='store_true');group.add_argument('--check',type=Path)
    args=parser.parse_args();header,rows=derive()
    if args.write:(ROOT/'slot_group76_golden.h').write_text(header,encoding='utf8',newline='\n')
    else:assert args.check.read_text(encoding='utf8')==header,'Independent current group model changed'
    print(json.dumps(rows));return 0

if __name__=='__main__':raise SystemExit(main())
