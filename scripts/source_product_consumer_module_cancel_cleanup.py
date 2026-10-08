#!/usr/bin/env python3
"""Encode cancellation-only cleanup output with a captured completion Cell."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def packet():
    def w(*v): return struct.pack('<'+'I'*len(v),*v)
    def blob(v): return w(len(v))+v
    def op(code,t=0,a=0,b=0,target=0,other=0,value=0):
        return w(code,t,a,b,target,other)+struct.pack('<Q',value)+w(0,0)
    def function(name,parameters,result,blocks,instructions,operands):
        value=blob(name)+w(len(parameters),*parameters,result,len(blocks))
        for block in blocks:value+=w(*block)
        return value+w(len(instructions))+b''.join(instructions)+w(len(operands),*operands)
    body=w(0,6,1)
    for i in range(4):
        if i==1:
            instructions=[op(2,2,value=2),op(1,1),op(59,256,a=1),op(24,b=1),
                op(108,a=1,b=1,target=1,value=5),op(29),op(1,1,value=1),op(61,a=2,b=6),op(33)]
            body+=function(b'left_init',[],0,[(0,5,0,0),(5,4,0,5)],instructions,[0,2])
        else:
            body+=function(('init'+str(i)).encode(),[],0,[(0,3,0,0)],
                           [op(2,2,value=i+1),op(24,b=1),op(33)],[0])
    body+=function(b'entry',[],2,[(0,2,0,0)],[op(2,2,value=42),op(33)],[])
    body+=function(b'left_cleanup',[256],0,[(0,2,0,0),(2,1,0,0),(3,3,0,0)],
                   [op(60,1),op(32,a=1,target=1,other=2),op(33),op(2,2,value=2),op(24,b=1),op(33)],[4])
    body+=w(4,0,0,3,4)
    for i,(name,deps) in enumerate([(b'base',[]),(b'left',[0]),(b'right',[0]),(b'main',[1,2])]):
        body+=blob(name)+w(len(deps),*deps,i)
    for i,owner in enumerate((0,1,2,3,3,1)):body+=w(owner,0,0,0,2 if i==5 else 0,0,0,0,0)
    body+=w(0,0,1,0,0,3,0,1,0,0)  # implementations, generics, types, Cell<Bool>, defaults, provenance
    head=b'XRCHK\0\0\0'+w(25,67,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body


def artifacts():
    data=packet();lines=['/* Complete independent input; cleanup emits only before the completion Cell is set. */',
                       'static const uint8_t module_cancel_cleanup_packet[] = {']
    for off in range(0,len(data),32):lines.append('    '+','.join(f'0x{x:02x}' for x in data[off:off+32])+',')
    lines+=['};','typedef struct ModuleCancelCleanupCase {',
            '    const char *name; const uint8_t *bytes; size_t length; unsigned action[2];',
            '} ModuleCancelCleanupCase;','static const ModuleCancelCleanupCase module_cancel_cleanup_cases[] = {']
    cases=[]
    for name,action in [('resume',[0,0]),('cancel',[1,0]),('destroy',[2,0]),('cancel_output_refusal',[3,0]),('destroy_output_refusal',[4,0])]:
        cases.append(dict(name=name,action=action));lines.append('    {"%s",module_cancel_cleanup_packet,sizeof(module_cancel_cleanup_packet),{%s}},' % (name,','.join(map(str,action))))
    lines+=['};','']
    return '\n'.join(lines),dict(schema=1,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),normal_stdout_hex=b'1\n2\n3\n4\n'.hex(),cancel_stdout_hex=b'1\n2\n2\n'.hex(),refused_stdout_hex=b'1\n2\n'.hex(),cases=cases)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('write','check'));args=p.parse_args();header,manifest=artifacts()
    for path,text in {DIR/'module_cancel_cleanup_cases.h':header,DIR/'module_cancel_cleanup_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'independent cancel cleanup fixture differs: {path}')
    print('Module cancel cleanup: captured Cell, explicit helper, five independent action scenarios')
