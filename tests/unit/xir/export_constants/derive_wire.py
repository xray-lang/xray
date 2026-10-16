"""Encode all exported/private slot packet fields without a product writer."""
from pathlib import Path
import argparse,hashlib,re,struct
word=lambda *v:struct.pack('<'+'I'*len(v),*v)
def blob(value):return word(len(value))+value
def op(code,kind=0,arg=0,immediate=0):return word(code,kind,arg,0,0,0)+struct.pack('<q',immediate)+word(0,0)
def function(name,result,ops):
    return blob(name)+word(0,result,1,0,len(ops),0,0,len(ops))+b''.join(ops)+word(0)
def model():
    body=word(0,3,1)
    body+=function(b'init',0,[op(2,2,immediate=42),op(5),op(2,2,immediate=7),op(5,arg=2,immediate=1),op(33)])
    body+=function(b'root_init',0,[op(33)])
    read_at=64+len(body)+4+len(b'read')+4+4+4+16+4
    body+=function(b'read',2,[op(4,2),op(33)])
    decl_at=64+len(body)
    body+=word(2,2,2,1,2)
    body+=blob(b'lib')+word(0,0,0)+blob(b'root')+word(1,0,1,0)
    body+=word(0,0,0,0,0,0,0,0,0,0)+word(1,0,0,0,0,0,0,0,0,0)+word(1,1,0,0,0,0,0,0,0,0)
    slot_at=64+len(body)
    body+=word(0,2,0,1,0)+word(0,2,0,0,0)
    literal_at=64+len(body)
    body+=blob(b'answer')+blob(b'secret')+word(0)
    body+=word(0,0,0,0,0,0,0)
    head=b'XRCHK\0\0\0'+word(31,79,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body,dict(SLOT=slot_at,DECL=decl_at,READ=read_at,LITERAL=literal_at)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    data,positions=model()
    text='/* Complete independently specified slot and visibility packet fields. */\n'
    text+='enum {'+','.join('CONSTANT_'+k+'='+str(v) for k,v in positions.items())+'};\n'
    text+='static const uint8_t constant_wire[] = {\n'
    text+=''.join('    '+','.join('0x%02x'%v for v in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};\n'
    path=Path(__file__).with_name('wire_golden.h')
    if args.write:path.write_bytes(text.encode())
    else:assert path.read_text()==text
    retired=Path(__file__).with_name('retired29_slot.h').read_text()
    raw=bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',retired))
    assert len(raw)==706 and hashlib.sha256(raw).hexdigest()=='f4cbd63d606ca10c84d738f4d29591d2f04df81a2bec7cb2210f61981fd17671'
    assert struct.unpack_from('<II',raw,8)==(29,76)
    assert struct.unpack_from('<Q',raw,24)[0]==len(raw)-64
    assert hashlib.sha256(raw[:32]+raw[64:]).digest()==raw[32:64]
    prior=Path(__file__).with_name('retired30_constants.h').read_text()
    raw=bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',prior))
    assert len(raw)==796 and hashlib.sha256(raw).hexdigest()=='7743f95438acc6ae35cf9851a864783d95227530b6b174a06e61608e540678b3'
    assert struct.unpack_from('<II',raw,8)==(30,77)
    assert hashlib.sha256(raw[:32]+raw[64:]).digest()==raw[32:64]
    print('Independent exported/private slot packet:',len(data),hashlib.sha256(data).hexdigest(),positions)
if __name__=='__main__':main()
