#!/usr/bin/env python3
"""Encode all original integer conversion pairs without the production codec."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'
TYPES = [('i8',5), ('u8',8), ('i16',6), ('u16',9), ('i32',7), ('u32',10), ('i64',2), ('u64',11)]
CASES = [('matrix',0,0), ('boolean_operand',1,3), ('boolean_result',2,3),
         ('nonzero_immediate',3,1), ('unused_second_argument',4,1), ('unused_target',5,1),
         ('inactive_type_argument',6,1), ('wrong_return_type',7,3)]


def encode(mutation):
    def w(*v): return struct.pack('<'+'I'*len(v),*v)
    def instruction(code,kind=0,arg0=0,arg1=0,target0=0,immediate=0,typearg0=0):
        return w(code,kind,arg0,arg1,target0,0)+struct.pack('<q',immediate)+w(typearg0,0)
    # No declarations;64 functions;zero module slots. Value0 is the parameter,
    # value1 is the conversion result. Return uses that result,never a new constant.
    body=w(0,64,0)
    for index,((source,source_type),(target,target_type)) in enumerate((a,b) for a in TYPES for b in TYPES):
        name=('conv_'+source+'_'+target).encode()
        changed=mutation if index==0 else 0
        parameter=1 if changed==1 else source_type
        result=1 if changed in (2,7) else target_type
        conversion_type=1 if changed==2 else target_type
        body+=w(len(name))+name+w(1,parameter,result,1,0,2,0,0,2)
        body+=instruction(62,conversion_type,arg1=int(changed==4),target0=int(changed==5),
                          immediate=int(changed==3),typearg0=int(changed==6))
        body+=instruction(33,arg0=1)+w(0)
    # No generics,type nodes,nominals,interfaces,default bindings or binding owner.
    body+=w(0,0,0,0,0,0)
    header=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return header+hashlib.sha256(header+body).digest()+body


def artifacts():
    lines=['/* Independently encoded complete64-pair integer conversion tables. */']
    manifest=dict(schema=1,identity='Checked25/semantic67',pairs=[dict(source=a,source_type=at,target=b,target_type=bt,name='conv_'+a+'_'+b) for a,at in TYPES for b,bt in TYPES],cases=[])
    for index,(name,mutation,expected) in enumerate(CASES):
        packet=encode(mutation)
        lines.append(f'static const uint8_t integer_conversion_packet_{index}[] = {{')
        for offset in range(0,len(packet),32):lines.append('    '+','.join(f'0x{x:02x}' for x in packet[offset:offset+32])+',')
        lines.append('};')
        manifest['cases'].append(dict(name=name,mutation=mutation,expected=expected,bytes=len(packet),sha256=hashlib.sha256(packet).hexdigest()))
    lines+=['typedef struct IntegerConversionCase {','    const char *name; const uint8_t *bytes; size_t length; unsigned mutation; XrXirStatus expected;','} IntegerConversionCase;','static const IntegerConversionCase integer_conversion_cases[] = {']
    for index,row in enumerate(manifest['cases']):lines.append(f'    {{"{row["name"]}",integer_conversion_packet_{index},sizeof(integer_conversion_packet_{index}),{row["mutation"]},{row["expected"]}}},')
    lines+=['};','typedef struct IntegerConversionPair { const char *name; XrXirType source, target; } IntegerConversionPair;','static const IntegerConversionPair integer_conversion_pairs[] = {']
    for row in manifest['pairs']:lines.append(f'    {{"{row["name"]}",{row["source_type"]},{row["target_type"]}}},')
    lines+=['};','']
    manifest.update(matrix='64original integer type pairs;8Built+8wire inputs;empty+occupied output=32transitions/config',boundary='Built/Checked admission,owned snapshots and physical release. Old explicit owner bit and immediate presence tag lack current fields;wholelegacy staysOPEN. No SourceProduct,Lowered,Program or runtime execution claimed.')
    return '\n'.join(lines),manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('write','check'));args=parser.parse_args()
    header,manifest=artifacts()
    for path,value in {DIR/'integer_conversion_cases.inc.c':header,DIR/'integer_conversion_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(value,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=value:raise ValueError(f'independent integer conversion input differs: {path}')
    print('Integer conversions:64pairs;8Built/8wire inputs;32transitions;legacy owner/immediate tag duties OPEN')
