#!/usr/bin/env python3
"""Encode a complete independent diamond with original numbered output groups."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def packet():
    def words(*v): return struct.pack('<'+'I'*len(v), *v)
    def blob(v): return words(len(v))+v
    def ins(op, type_id, value=0, count=0):
        return words(op,type_id,0,count,0,0)+struct.pack('<Q',value)+words(0,0)
    body = words(0,5,1)
    for i in range(4):
        count = 4 if i == 1 else 3
        body += blob(('init'+str(i)).encode())+words(0,0,1,0,count,0,0,count)
        body += ins(2,2,i+1)+ins(24,0,count=1)
        if i == 1: body += ins(29,0)
        body += ins(33,0)+words(1,0)
    body += blob(b'entry')+words(0,2,1,0,2,0,0,2)+ins(2,2,42)+ins(33,0)+words(0)
    body += words(4,0,0,3,4)
    for i,(name,deps) in enumerate([(b'base',[]),(b'left',[0]),(b'right',[0]),(b'main',[1,2])]):
        body += blob(name)+words(len(deps),*deps,i)
    for owner in (0,1,2,3,3): body += words(owner,*([0]*8))
    body += words(*([0]*7))
    head=b'XRCHK\0\0\0'+words(25,67,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body


def artifacts():
    data=packet();lines=['/* Independent full diamond; each initializer prints its original 1-based number. */',
                       'static const uint8_t module_suspension_packet[] = {']
    for off in range(0,len(data),32): lines.append('    '+','.join(f'0x{x:02x}' for x in data[off:off+32])+',')
    lines += ['};','typedef struct ModuleSuspensionCase {',
              '    const char *name; const uint8_t *bytes; size_t length; unsigned action[2];',
              '} ModuleSuspensionCase;','static const ModuleSuspensionCase module_suspension_cases[] = {']
    cases=[]
    for name,actions in [('resume_both',[0,0]),('cancel_peer_resume',[1,0]),('destroy_peer_resume',[2,0])]:
        cases.append(dict(name=name,action=actions))
        lines.append('    {"%s",module_suspension_packet,sizeof(module_suspension_packet),{%s}},' % (name,','.join(map(str,actions))))
    lines += ['};','']
    return '\n'.join(lines),dict(schema=1,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),stdout_hex=b'1\n2\n3\n4\n'.hex(),cases=cases)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=('write','check'));args=p.parse_args()
    header,manifest=artifacts()
    for path,text in {DIR/'module_suspension_cases.h':header,DIR/'module_suspension_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'independent module output fixture differs: {path}')
    print('Module suspension: complete diamond, exact numbered output, resume/cancel/destroy with independent peer')
