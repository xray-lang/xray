#!/usr/bin/env python3
"""Encode complete independent Checked module-slot declarations and reject cases."""
import argparse
import copy
import hashlib
import json
import struct
from pathlib import Path
DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def model():
    return dict(slots=[[i, t, 0] for i in range(4) for t in (2, 3)],
                initializers=[0, 1, 2, 3], type_kind=0)


def encode(m):
    def w(*v): return struct.pack('<'+'I'*len(v), *v)
    def blob(v): return w(len(v))+v
    def op(code, t=0, value=0): return w(code,t,0,0,0,0)+struct.pack('<Q',value)+w(0,0)
    body=w(0,5,1)
    for i in range(4):
        body+=blob(('init'+str(i)).encode())+w(0,0,1,0,1,0,0,1)+op(33)+w(0)
    body+=blob(b'entry')+w(0,2,1,0,2,0,0,2)+op(2,2,42)+op(33)+w(0)
    body+=w(4,len(m['slots']),0,3,4)
    for i,(name,deps) in enumerate([(b'base',[]),(b'left',[0]),(b'right',[0]),(b'main',[1,2])]):
        body+=blob(name)+w(len(deps),*deps,m['initializers'][i])
    for owner in (0,1,2,3,3): body+=w(owner,0,0,0,0,0,0,0,0)
    for slot in m['slots']: body+=w(*slot)
    body+=w(0,0,int(bool(m['type_kind'])),0,0)
    if m['type_kind']==6: body+=w(6,0,1,2)
    elif m['type_kind']==3: body+=w(3,0,2)
    body+=w(0,0)
    head=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body


def cases():
    rows=[('immutable_slots',model(),'XR_XIR_OK')]
    for name,index,field,value in [('root_mutable',6,2,1),('bool_slot',0,1,1),('unit_slot',0,1,0)]:
        m=model();m['slots'][index][field]=value;rows.append((name,m,'XR_XIR_OK'))
    m=model();m['type_kind']=6;m['slots'][0][1]=256;rows.append(('tuple_slot',m,'XR_XIR_OK'))
    m=model();m['slots'][0],m['slots'][1]=m['slots'][1],m['slots'][0];rows.append(('reordered_dense_slots',m,'XR_XIR_OK'))
    m=model();m['slots']=[];rows.append(('no_slots',m,'XR_XIR_OK'))
    m=model();m['slots'][6]=[3,0,1];rows.append(('root_mutable_unit',m,'XR_XIR_OK'))
    for name,field,value,status in [
        ('owner_out_of_range',0,4,'XR_XIR_BAD_STRUCTURE'),
        ('owner_maximum',0,0xffffffff,'XR_XIR_BAD_STRUCTURE'),
        ('mutability_two',2,2,'XR_XIR_BAD_STRUCTURE'),
        ('mutability_maximum',2,0xffffffff,'XR_XIR_BAD_STRUCTURE'),
        ('library_mutable',2,1,'XR_XIR_BAD_STRUCTURE'),
        ('reserved_type_four',1,4,'XR_XIR_BAD_TYPE'),
        ('unknown_type_127',1,127,'XR_XIR_BAD_TYPE'),
        ('unknown_type_255',1,255,'XR_XIR_BAD_TYPE'),
        ('missing_constructed_pool',1,256,'XR_XIR_BAD_TYPE'),
        ('open_type_parameter',1,65536,'XR_XIR_BAD_TYPE')]:
        m=model();m['slots'][0][field]=value;rows.append((name,m,status))
    m=model();m['type_kind']=6;m['slots'][0][1]=257;rows.append(('constructed_out_of_range',m,'XR_XIR_BAD_TYPE'))
    m=model();m['type_kind']=3;m['slots'][0][1]=256;rows.append(('cell_slot',m,'XR_XIR_BAD_TYPE'))
    for name,initializer in [('missing_initializer',0xffffffff),('foreign_initializer',1)]:
        m=model();m['initializers'][0]=initializer;rows.append((name,m,'XR_XIR_BAD_STRUCTURE'))
    assert len(rows)==22
    return rows


def artifacts():
    lines=['/* Complete independent slot packets; no product writer constructs input. */']
    manifest=dict(schema=1,controls=8,rejects=14,cases=[])
    for i,(name,m,status) in enumerate(cases()):
        data=encode(m);lines.append(f'static const uint8_t module_slot_packet_{i}[] = {{')
        for off in range(0,len(data),32):lines.append('    '+','.join(f'0x{x:02x}' for x in data[off:off+32])+',')
        lines.append('};');manifest['cases'].append(dict(name=name,expected=status,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),**copy.deepcopy(m)))
    assert len({c['sha256'] for c in manifest['cases']})==22
    lines+=['typedef struct ModuleSlotCase {','    const char *name; const uint8_t *bytes; size_t length; XrXirStatus expected;',
            '    uint32_t count, slots[8][3], type_kind;','} ModuleSlotCase;','static const ModuleSlotCase module_slot_cases[] = {']
    for i,row in enumerate(manifest['cases']):
        slots=row['slots'] or [[0,0,0]];value=','.join('{'+','.join(str(v)+'u' for v in slot)+'}' for slot in slots)
        lines.append('    {"%s",module_slot_packet_%d,sizeof(module_slot_packet_%d),%s,%d,{%s},%d},' % (row['name'],i,i,row['expected'],len(row['slots']),value,row['type_kind']))
    lines+=['};',''];return '\n'.join(lines),manifest


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('write','check'));args=p.parse_args();header,manifest=artifacts()
    for path,text in {DIR/'module_slot_cases.h':header,DIR/'module_slot_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'independent module-slot fixture differs: {path}')
    print('Module slots: 22 complete independent packets, 8 controls, 14 rejects')
