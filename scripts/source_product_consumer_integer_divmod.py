#!/usr/bin/env python3
"""Encode every original integer quotient and remainder signature independently."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DIR=Path(__file__).resolve().parents[1]/'tests/unit/xir/product_consumers'
TYPES=[('i8',5),('u8',8),('i16',6),('u16',9),('i32',7),('u32',10),('i64',2),('u64',11)]
CASES=[('matrix',0,0),('boolean_lhs',1,3),('boolean_rhs',2,3),('boolean_result',3,3),
       ('inactive_selector',4,1),('invalid_panic_target',5,1),('missing_rhs_value',6,4),
       ('wrong_return_type',7,3),('inactive_type_argument',8,1),('unused_target',9,1)]


def encode(mutation):
    def w(*v):return struct.pack('<'+'I'*len(v),*v)
    def instruction(code,kind=0,arg0=0,arg1=0,target=0,immediate=0,typearg=0):
        return w(code,kind,arg0,arg1,target,0)+struct.pack('<q',immediate)+w(typearg,0)
    body=w(0,16,0)
    for index,(typename,kind,remainder) in enumerate((n,t,r) for n,t in TYPES for r in range(2)):
        name=(('rem_' if remainder else 'div_')+typename).encode();changed=mutation if index==0 else 0
        lhs=1 if changed==1 else kind;rhs=1 if changed==2 else kind
        result=1 if changed in (3,7) else kind;operation_type=1 if changed==3 else kind
        body+=w(len(name))+name+w(2,lhs,rhs,result,1,0,2,int(changed==5),0,2)
        body+=instruction(46 if remainder else 45,operation_type,arg1=4294967295 if changed==6 else 1,
                          target=int(changed==9),immediate=2 if changed==4 else 0,typearg=int(changed==8))
        body+=instruction(33,arg0=2)+w(0)
    body+=w(0,0,0,0,0,0)
    header=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return header+hashlib.sha256(header+body).digest()+body


def artifacts():
    lines=['/* Independent Checked quotients and remainders for all8integer types. */']
    manifest=dict(schema=1,identity='Checked25/semantic67',functions=[dict(name=('rem_' if r else 'div_')+n,type=t,remainder=r,operation=46 if r else 45) for n,t in TYPES for r in range(2)],cases=[])
    for index,(name,mutation,expected) in enumerate(CASES):
        packet=encode(mutation);lines.append(f'static const uint8_t integer_divmod_packet_{index}[] = {{')
        for offset in range(0,len(packet),32):lines.append('    '+','.join(f'0x{x:02x}' for x in packet[offset:offset+32])+',')
        lines.append('};');manifest['cases'].append(dict(name=name,mutation=mutation,expected=expected,bytes=len(packet),sha256=hashlib.sha256(packet).hexdigest()))
    lines+=['typedef struct IntegerDivmodCase {','    const char *name; const uint8_t *bytes; size_t length; unsigned mutation; XrXirStatus expected;','} IntegerDivmodCase;','static const IntegerDivmodCase integer_divmod_cases[] = {']
    for index,row in enumerate(manifest['cases']):lines.append(f'    {{"{row["name"]}",integer_divmod_packet_{index},sizeof(integer_divmod_packet_{index}),{row["mutation"]},{row["expected"]}}},')
    lines+=['};','typedef struct IntegerDivmodFunction { const char *name; XrXirType type; XrXirOp operation; } IntegerDivmodFunction;','static const IntegerDivmodFunction integer_divmod_functions[] = {']
    for row in manifest['functions']:lines.append(f'    {{"{row["name"]}",{row["type"]},{row["operation"]}}},')
    lines+=['};',''];assert len(lines)<=3000
    manifest.update(matrix='All16original quotient/remainder signatures;10Built+10wire inputs;empty+occupied output=40transitions/config',boundary='Admission and detached ownership only. Old panic ABI,owner bit,operation selector andarity fields have no identical encoding. WholelegacyOPEN;no runtime arithmetic or panics proved.')
    return '\n'.join(lines),manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('write','check'));args=parser.parse_args();code,manifest=artifacts()
    for path,value in {DIR/'integer_divmod_cases.inc.c':code,DIR/'integer_divmod_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(value,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=value:raise ValueError(f'independent integer divmod input differs: {path}')
    print('Integer divmod:16functions;10Built/10wire inputs;40transitions;legacy panicABI/owner/tag/arity OPEN')
