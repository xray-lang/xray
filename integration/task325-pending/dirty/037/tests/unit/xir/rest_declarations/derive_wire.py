"""Encode every rest declaration and authentic function-reference packet field."""
from pathlib import Path
import argparse,hashlib,struct

word=lambda *v:struct.pack('<'+'I'*len(v),*v)
def blob(value):return word(len(value))+value
def op(code,kind=0,a=0,b=0,immediate=0):return word(code,kind,a,b,0,0)+struct.pack('<q',immediate)+word(0,0)
def function(name,parameters,result,ops,operands=()):
    return blob(name)+word(len(parameters),*parameters,result,1,0,len(ops),0,0,len(ops))+b''.join(ops)+word(len(operands),*operands)

def model():
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

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    data,positions=model();text='/* Independently specified declaration, callable and real Array call fields. */\n'
    text+='enum {'+','.join('REST_'+k+'='+str(v) for k,v in positions.items())+'};\n'
    text+='static const uint8_t rest_wire[] = {\n'+''.join('    '+','.join('0x%02x'%v for v in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};\n'
    target=Path(__file__).with_name('wire81_golden.h')
    if args.write:target.write_bytes(text.encode())
    else:assert target.read_text()==text
    print('Independent rest packet:',len(data),hashlib.sha256(data).hexdigest(),positions)
if __name__=='__main__':main()
