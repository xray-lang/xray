#!/usr/bin/env python3
"""Encode original mutable class state and explicit root-owned controls."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def packet(mutable, distinct, owner, helper=False):
    def w(*v):return struct.pack('<'+'I'*len(v),*v)
    def blob(v):return w(len(v))+v
    def op(code,t=0,a=0,b=0,value=0):return w(code,t,a,b,0,0)+struct.pack('<Q',value)+w(0,0)
    def fn(name,result,ins,operands=(),parameters=()):
        return blob(name)+w(len(parameters),*parameters,result,1,0,len(ins),0,0,len(ins))+b''.join(ins)+w(len(operands),*operands)
    body=w(0,10 if helper else 9,1);ins=[];operands=[]
    for slot in (1,0,3,2):
        k=len(ins);offset=len(operands);operands.append(k)
        ins += [op(3,3),op(118,256,a=offset,b=1),op(18,256,a=k+1),op(5,a=k+2,value=slot),op(3,3,value=1),op(120,a=k+1,b=k+4)]
        if helper:
            ins.append(op(28,a=len(operands),b=1,value=9));operands.append(k+1)
    ins.append(op(33))
    for i in range(4):body+=fn(('init'+str(i)).encode(),0,ins if i==owner else [op(33)],operands if i==owner else [])
    body+=fn(b'program_entry',2,[op(2,2),op(33)])
    for slot in range(4):body+=fn(('inspect'+str(slot)).encode(),256,[op(4,256,value=slot),op(33)])
    if helper:body+=fn(b'consume',0,[op(3,3),op(33)],parameters=(256,))
    body+=w(4,4,2,3,4)
    for i,(name,deps) in enumerate([(b'base',[]),(b'left',[0]),(b'right',[0]),(b'main',[1,2])]):body+=blob(name)+w(len(deps),*deps,i)
    for i,module in enumerate((0,1,2,3,3,owner,owner,owner,owner)+((owner,) if helper else ())):body+=w(module,int(4<=i<9),0,0,0,0,0,0,0)
    for _ in range(4):body+=w(owner,256,int(mutable))
    body+=blob(b'x')+blob(b'changed' if distinct else b'x')
    body+=w(0,0,1,1,0) # implementations, generics, type nodes, nominals, interfaces
    body+=w(4,0,0,0,1,3) # closed class, declaration0, no type args, one string field
    body+=blob(b'main' if owner==3 else b'base')+blob(b'ModuleState')+w(1,2,1,0)+bytes(32)
    body+=w(0,1)+blob(b'text')+w(3,4)+w(0,0,0) # no binders, mutable field, no variants/defaults/provenance
    head=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body


def artifacts():
    rows=[('root_original_bytes',True,False,3,False),('root_distinct_replacement',True,True,3,False),('original_base_mutable_shape',True,False,0,False),('root_helper',True,False,3,True),('original_base_helper',True,False,0,True)]
    lines=['/* Original mutable slots retain positive expectations; root controls are separate. */'];manifest=dict(schema=1,cases=[])
    for i,(name,mutable,distinct,owner,helper) in enumerate(rows):
        data=packet(mutable,distinct,owner,helper);lines.append(f'static const uint8_t module_class_state_packet_{i}[] = {{')
        for off in range(0,len(data),32):lines.append('    '+','.join(f'0x{x:02x}' for x in data[off:off+32])+',')
        lines.append('};');manifest['cases'].append(dict(name=name,owner=owner,mutable=mutable,helper=helper,functions=10 if helper else 9,field='changed' if distinct else 'x',bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),expected='XR_XIR_OK'))
    lines+=['typedef struct ModuleClassStateCase {','    const char *name; const uint8_t *bytes; size_t length; const char *field; unsigned functions;','} ModuleClassStateCase;','static const ModuleClassStateCase module_class_state_cases[] = {']
    for i,row in enumerate(manifest['cases']):lines.append('    {"%s",module_class_state_packet_%d,sizeof(module_class_state_packet_%d),"%s",%d},' % (row['name'],i,i,row['field'],row['functions']))
    lines+=['};',''];manifest['oracle']=dict(instances=2,repeats=24,publication_order=[1,0,3,2],release_order=[2,3,0,1],objects=4,policy=['release all with Instance','retain last constructed object after Instance/Program/Lowered death'])
    return '\n'.join(lines),manifest


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('write','check'));args=p.parse_args();header,manifest=artifacts()
    for path,text in {DIR/'module_class_state_cases.h':header,DIR/'module_class_state_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'independent class state input differs: {path}')
    print('Module class state: original mutable shapes and three root controls with private helper; publication1,0,3,2 reverse release2,3,0,1')
