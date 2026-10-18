"""Encode every rest declaration and authentic function-reference packet field."""
from pathlib import Path
import argparse,hashlib,re,struct

word=lambda *v:struct.pack('<'+'I'*len(v),*v)
def blob(value):return word(len(value))+value
def op(code,kind=0,a=0,b=0,immediate=0):return word(code,kind,a,b,0,0)+struct.pack('<q',immediate)+word(0,0)
def function(name,parameters,result,ops,operands=()):
    return blob(name)+word(len(parameters),*parameters,result,1,0,len(ops),0,0,len(ops))+b''.join(ops)+word(len(operands),*operands)

def historical_model():
    body=word(0,3,1)
    body+=function(b'init',(),0,[op(33)])
    body+=function(b'read',(256,),2,[op(2,2,immediate=42),op(33,a=1)])
    entry=64+len(body)+len(blob(b'entry'))+8+4+16+4
    body+=function(b'entry',(),2,[op(2,2,immediate=19),op(2,2,immediate=23),op(73,256,0,2),
        op(28,2,2,1,1),op(57,257,immediate=1),op(58,2,3,1,4),op(25,2,3,5),op(33,a=6)],(0,1,2,2))
    body+=word(1,0,0,0,2)+blob(b'root')+word(0,0,0)
    identity=64+len(body)
    body+=word(0,0,0,0,0,0,0,0,0,0)+word(0,1,0,0,0,0,0,0,0,1)+word(0,1,0,0,0,0,0,0,0,0)
    body+=word(0,0,3,0,0)
    array=64+len(body);body+=word(2,0,2)
    callable=64+len(body);body+=word(1,0,1,256,0,2,18)
    body+=word(1,0,1,256,0,2,2)
    body+=word(0,0,0)
    head=b'XRCHK\0\0\0'+word(33,81,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body,dict(ENTRY=entry,IDENTITY=identity,ARRAY=array,CALLABLE=callable)


# The historical fixture remains an independent complete field specification.
# Re-encoding adds the current named fields through the complete wire grammar.
import sys
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'composed_wire'))
from model import decode,encode

def literal(text,name):
    match=re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\};',text,re.S)
    assert match,name
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[1]))

def model():
    historical,_=historical_model()
    directory=Path(__file__).resolve().parent
    assert literal((directory/'wire81_golden.h').read_text(encoding='utf-8'),'rest_wire')==historical
    facts,_=decode(historical,'P10')
    packet,offsets=encode(facts)
    assert encode(decode(packet,'current')[0])[0]==packet
    expected=literal((directory.parent/'composed_wire/packet_004.inc.c').read_text(encoding='utf-8'),'packet_004_current')
    assert packet==expected
    names={"ENTRY":"module.functions[2].instructions[0].op","IDENTITY":"module.declarations.identities[0].module","ARRAY":"module.nodes[0].kind","CALLABLE":"module.nodes[1].kind"}
    return packet,{name:offsets[key][0] for name,key in names.items()}

def fields_header(positions):
    text='#ifndef XR_REST_CURRENT_FIELDS_H\n#define XR_REST_CURRENT_FIELDS_H\n'
    text+='#include "../composed_wire/packet_004.inc.c"\n'
    text+='enum {'+','.join('REST_'+name+'='+str(value) for name,value in positions.items())+'};\n'
    return text+'#endif\n'

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    data,positions=model();text=fields_header(positions)
    target=Path(__file__).with_name('current_fields.h')
    if args.write:target.write_bytes(text.encode('utf-8'))
    else:assert target.read_text(encoding='utf-8')==text
    print('Independent rest packet:',len(data),hashlib.sha256(data).hexdigest(),positions)
if __name__=='__main__':main()
