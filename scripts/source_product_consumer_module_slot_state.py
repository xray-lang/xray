#!/usr/bin/env python3
"""Encode original base-owned state shapes and explicit root-owned execution controls."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def packet(text, owner):
    def w(*v):return struct.pack('<'+'I'*len(v),*v)
    def blob(v):return w(len(v))+v
    def op(code,t=0,a=0,b=0,value=0):return w(code,t,a,b,0,0)+struct.pack('<Q',value)+w(0,0)
    def fn(name,result,instructions,operands):
        return blob(name)+w(0,result,1,0,len(instructions),0,0,len(instructions))+b''.join(instructions)+w(len(operands),*operands)
    t=3 if text else 2;body=w(0,6,1)
    for i in range(4):
        instructions=[op(3,3) if text else op(2,2,value=40),op(5),op(33)] if i==owner else [op(33)]
        body+=fn(('init'+str(i)).encode(),0,instructions,[])
    if text:instructions=[op(4,3),op(3,3,value=1),op(6,a=1),op(24,b=1),op(33)]
    else:instructions=[op(4,2),op(2,2,value=1),op(25,2,b=1),op(6,a=2),op(4,2),op(33,a=4)]
    body+=fn(b'entry',0 if text and owner==0 else t,instructions,[0] if text else [])
    body+=fn(b'program_entry',2,[op(2,2),op(33)],[])
    body+=w(4,1,2 if text else 0,3,5)
    for i,(name,deps) in enumerate([(b'base',[]),(b'left',[0]),(b'right',[0]),(b'main',[1,2])]):body+=blob(name)+w(len(deps),*deps,i)
    for i,module in enumerate((0,1,2,3,3,3)):body+=w(module,int(i==4),0,0,0,0,0,0,0)
    body+=w(owner,t,1)
    if text:body+=blob(b'module-owned-text')+blob(b'changed')
    body+=w(0,0,0,0,0,0,0)
    head=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body


def artifacts():
    rows=[('root_integer_control',False,3),('root_string_control',True,3),('original_base_integer_shape',False,0),('original_base_string_shape',True,0)]
    lines=['/* Complete independent inputs; root controls never replace base-owned original obligations. */'];manifest=dict(schema=1,cases=[])
    for i,(name,text,owner) in enumerate(rows):
        data=packet(text,owner);lines.append(f'static const uint8_t module_slot_state_packet_{i}[] = {{')
        for off in range(0,len(data),32):lines.append('    '+','.join(f'0x{x:02x}' for x in data[off:off+32])+',')
        lines.append('};');manifest['cases'].append(dict(name=name,owner=owner,slot_type=3 if text else 2,type=0 if text and owner==0 else 3 if text else 2,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),original_positive_expected='XR_XIR_OK',execution_scope='root control' if owner==3 else 'original ownership shape admission only'))
    lines+=['typedef struct ModuleSlotStateCase {','    const char *name; const uint8_t *bytes; size_t length; XrXirType result;',
            '} ModuleSlotStateCase;','static const ModuleSlotStateCase module_slot_state_cases[] = {']
    for i,row in enumerate(manifest['cases']):lines.append('    {"%s",module_slot_state_packet_%d,sizeof(module_slot_state_packet_%d),%d},' % (row['name'],i,i,row['type']))
    lines+=['};',''];manifest['execution_oracle']=dict(instances=2,repeats=24,integer_results=list(range(41,65)),text_output=['module-owned-text\n']+['changed\n']*23)
    return '\n'.join(lines),manifest


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('write','check'));args=p.parse_args();header,manifest=artifacts()
    for path,text in {DIR/'module_slot_state_cases.h':header,DIR/'module_slot_state_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'independent state input differs: {path}')
    print('Module slot state: two original base-owned shapes and two explicit root controls,24 calls per instance')
