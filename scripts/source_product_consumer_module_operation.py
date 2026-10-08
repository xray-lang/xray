#!/usr/bin/env python3
"""Encode independent slot-operation authority and exact type expectations."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def cases():
    rows = []
    def add(name, expected=0, **changes):
        row = dict(name=name, expected=expected, owner=3, caller=3, mutable=1,
                   slot_type=2, value_type=2, operation=5, result_type=0, ordinal=0)
        row.update(changes); rows.append(row)
    add('root_mutable_init')
    add('root_const_init', mutable=0)
    add('root_mutable_store', operation=6)
    add('original_base_const_init', owner=0, caller=0, mutable=0)
    add('original_base_mutable_init', owner=0, caller=0)
    add('original_base_mutable_store', owner=0, caller=0, operation=6)
    add('root_bool_init', slot_type=1, value_type=1)
    add('foreign_initializer', 1, caller=0)
    add('noninitializer_init', 1, caller=4)
    add('load_type_mismatch', 3, operation=4, result_type=1)
    add('init_value_mismatch', 3, value_type=1)
    add('store_value_mismatch', 3, operation=6, value_type=1)
    add('root_const_store', 1, mutable=0, operation=6)
    add('base_const_store', 1, owner=0, caller=0, mutable=0, operation=6)
    add('foreign_load', 1, caller=0, operation=4, result_type=2)
    add('foreign_store', 1, caller=0, operation=6)
    add('slot_one_past_end', 1, ordinal=1)
    add('slot_u32_max', 1, ordinal=0xffffffff)
    add('slot_negative', 1, ordinal=0xffffffffffffffff)
    add('slot_missing_owner', 1, owner=4)
    return rows


def encode(m):
    def w(*v): return struct.pack('<'+'I'*len(v), *v)
    def blob(v): return w(len(v))+v
    def op(code, t=0, a=0, value=0):
        return w(code,t,a,0,0,0)+struct.pack('<Q',value)+w(0,0)
    def fn(name, result, ins):
        return blob(name)+w(0,result,1,0,len(ins),0,0,len(ins))+b''.join(ins)+w(0)
    body=w(0,5,1)
    for i in range(5):
        ins=[op(2,2,value=42),op(33)] if i==4 else [op(33)]
        if i==m['caller']:
            ins=[op(1 if m['value_type']==1 else 2,m['value_type'],value=1 if m['value_type']==1 else 40),
                 op(m['operation'],m['result_type'],value=m['ordinal']),op(33)]
        body+=fn(('entry' if i==4 else 'init'+str(i)).encode(),2 if i==4 else 0,ins)
    body+=w(4,1,0,3,4)
    for i,(name,deps) in enumerate([(b'base',[]),(b'left',[0]),(b'right',[0]),(b'main',[1,2])]):
        body+=blob(name)+w(len(deps),*deps,i)
    for i in range(5): body+=w(i if i<4 else 3,0,0,0,0,0,0,0,0)
    body+=w(m['owner'],m['slot_type'],m['mutable'])+w(0,0,0,0,0,0,0)
    head=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body


def artifacts():
    rows=cases(); manifest=dict(schema=1,cases=[])
    lines=['/* Original mutable library positives remain positive obligations. */']
    for i,row in enumerate(rows):
        packet=encode(row); lines.append(f'static const uint8_t module_operation_packet_{i}[] = {{')
        for off in range(0,len(packet),32): lines.append('    '+','.join(f'0x{x:02x}' for x in packet[off:off+32])+',')
        lines.append('};'); manifest['cases'].append(dict(**row,bytes=len(packet),sha256=hashlib.sha256(packet).hexdigest()))
    lines+=['typedef struct ModuleOperationCase {',
            '    const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected;',
            '    unsigned owner, caller, mutable, slot_type, value_type, operation, result_type;',
            '} ModuleOperationCase;','static const ModuleOperationCase module_operation_cases[] = {']
    for i,row in enumerate(rows):
        fields=','.join(str(row[k]) for k in ('expected','owner','caller','mutable','slot_type','value_type','operation','result_type'))
        lines.append(f'    {{"{row["name"]}",module_operation_packet_{i},sizeof(module_operation_packet_{i}),{fields}}},')
    lines+=['};',''];manifest['oracle']='20 complete inputs:7 positives including2 original mutable base positives;13 exact typed/structural rejects;each with empty and occupied output. Admission only; store before init is not runtime success.'
    return '\n'.join(lines),manifest


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('write','check'));args=p.parse_args()
    header,manifest=artifacts()
    for path,text in {DIR/'module_operation_cases.h':header,DIR/'module_operation_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'independent module operation input differs: {path}')
    print('Module operations:20 independent inputs;7 positive duties and13 exact rejects;no legacy retirement')
