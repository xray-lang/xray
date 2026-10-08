#!/usr/bin/env python3
"""Independently encode all integer bitwise signatures and exact admission failures."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DIR=Path(__file__).resolve().parents[1]/'tests/unit/xir/product_consumers'
TYPES=[('i8',5,-1),('u8',8,255),('i16',6,-1),('u16',9,65535),
       ('i32',7,-1),('u32',10,4294967295),('i64',2,-1),('u64',11,-1)]
OPS=[('and',51),('or',52),('xor',53),('not',53),('shl',54),('shr',55)]
CASES=[('matrix',0,0),('boolean_lhs',1,3),('boolean_rhs',2,3),('boolean_result',3,3),
       ('inactive_selector',4,1),('complement_mask_boolean',5,3),('missing_rhs_value',6,4),
       ('wrong_return_type',7,3),('inactive_type_argument',8,1),('unused_target',9,1)]


def encode(mutation):
    def w(*v):return struct.pack('<'+'I'*len(v),*v)
    def instruction(code,kind=0,arg0=0,arg1=0,target=0,immediate=0,typearg=0):
        return w(code,kind,arg0,arg1,target,0)+struct.pack('<q',immediate)+w(typearg,0)
    body=w(0,48,0)
    for index,(typename,kind,mask,opname,opcode) in enumerate((n,t,m,o,c) for n,t,m in TYPES for o,c in OPS):
        name=(opname+'_'+typename).encode();changed=mutation if index==0 else 0;complement=opname=='not';count=3 if complement else 2
        lhs=1 if changed==1 else kind;rhs=1 if changed==2 else kind
        result=1 if changed in (3,7) else kind;operation_type=1 if changed==3 else kind
        body+=w(len(name))+name+w(2,lhs,rhs,result,1,0,count,0,0,count)
        if complement:body+=instruction(2,1 if mutation==5 and index==3 else kind,immediate=mask)
        body+=instruction(opcode,operation_type,arg0=2 if complement else 0,
                          arg1=4294967295 if changed==6 else (0 if complement else 1),
                          target=int(changed==9),immediate=6 if changed==4 else 0,typearg=int(changed==8))
        body+=instruction(33,arg0=3 if complement else 2)+w(0)
    body+=w(0,0,0,0,0,0)
    header=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return header+hashlib.sha256(header+body).digest()+body


def artifacts():
    lines=['/* Independent Checked bitwise signatures; complement uses typed mask XOR. */']
    functions=[dict(name=o+'_'+n,type=t,operation=c,complement=o=='not',mask=m if o=='not' else 0) for n,t,m in TYPES for o,c in OPS]
    manifest=dict(schema=1,identity='Checked25/semantic67',functions=functions,cases=[])
    for index,(name,mutation,expected) in enumerate(CASES):
        packet=encode(mutation);lines.append(f'static const uint8_t integer_bitwise_packet_{index}[] = {{')
        for offset in range(0,len(packet),32):lines.append('    '+','.join(f'0x{x:02x}' for x in packet[offset:offset+32])+',')
        lines.append('};');manifest['cases'].append(dict(name=name,mutation=mutation,expected=expected,bytes=len(packet),sha256=hashlib.sha256(packet).hexdigest()))
    lines+=['typedef struct IntegerBitwiseCase {','    const char *name; const uint8_t *bytes; size_t length; unsigned mutation; XrXirStatus expected;','} IntegerBitwiseCase;','static const IntegerBitwiseCase integer_bitwise_cases[] = {']
    for index,row in enumerate(manifest['cases']):lines.append(f'    {{"{row["name"]}",integer_bitwise_packet_{index},sizeof(integer_bitwise_packet_{index}),{row["mutation"]},{row["expected"]}}},')
    lines+=['};','typedef struct IntegerBitwiseFunction {','    const char *name; XrXirType type; XrXirOp operation; bool complement; int64_t mask;','} IntegerBitwiseFunction;','static const IntegerBitwiseFunction integer_bitwise_functions[] = {']
    for row in functions:
        mask='INT64_C(4294967295)' if row['mask']==4294967295 else str(row['mask'])
        lines.append(f'    {{"{row["name"]}",{row["type"]},{row["operation"]},{"true" if row["complement"] else "false"},{mask}}},')
    lines+=['};',''];assert len(lines)<=3000
    manifest.update(matrix='All48original bitwise signatures;10Built+10wire inputs;empty+occupied output=40transitions/config',boundary='Admission and detached ownership only. Complement is mask XOR as emitted by Source. Old owner bit,selector andarity fields have no identical encoding;wholelegacyOPEN. Runtime bit values are not qualified.')
    return '\n'.join(lines),manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('write','check'));args=parser.parse_args();code,manifest=artifacts()
    for path,value in {DIR/'integer_bitwise_cases.inc.c':code,DIR/'integer_bitwise_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(value,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=value:raise ValueError(f'independent integer bitwise input differs: {path}')
    print('Integer bitwise:48functions;10Built/10wire inputs;40transitions;legacy owner/tag/arity OPEN')
