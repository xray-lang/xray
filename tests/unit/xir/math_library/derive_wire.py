"""Independent fixed math instruction packet fields; product writer unused."""
from pathlib import Path
import hashlib,json,struct
word=lambda *values:struct.pack('<'+'I'*len(values),*values)
def blob(value):return word(len(value))+value
def op(code,kind=0,a=0,b=0,selector=0):return word(code,kind,a,b,0,0)+struct.pack('<q',selector)+word(0,0)
def model():
 body=word(0,7,1);positions={}
 functions=[(b'init',(),0,[op(33)]),(b'pow',(13,13),13,[op(155,13,0,1,6),op(33,a=2)]),
  (b'floor',(13,),2,[op(156,2,0,0,2),op(33,a=1)]),
  (b'sqrt',(13,),13,[op(154,13,0,0,5),op(33,a=1)]),
  (b'random',(),13,[op(157,13,selector=18),op(33)]),
  (b'randomInt',(2,2),2,[op(158,2,0,1,19),op(33,a=2)]),
  (b'main',(),2,[op(2,2,selector=17),op(33)])]
 for name,parameters,result,operations in functions:
  body+=blob(name)+word(len(parameters),*parameters,result,1,0,len(operations),0,0,len(operations))
  positions[name.decode()]=64+len(body);body+=b''.join(operations)+word(0)
 body+=word(1,0,0,0,6)+blob(b'root')+word(0,0,0)
 body+=word(*([0]*10))
 for _ in range(5):body+=word(0,1,0,0,0,0,0,0,0,0)
 body+=word(*([0]*10))
 body+=word(0,0,0,0,0,0,0,0)
 header=b'XRCHK\0\0\0'+word(32,84,2,0)+struct.pack('<Q',len(body))
 return header+hashlib.sha256(header+body).digest()+body,positions
def render():
 data,positions=model()
 text='/* Complete independently specified fixed math instruction fields. */\n'
 text+='enum {'+','.join('MATH_'+name.upper()+'='+str(value) for name,value in positions.items())+'};\n'
 text+='static const uint8_t math_wire[] = {\n'+''.join('    '+','.join('0x%02x'%byte for byte in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};\n'
 return text,data
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
 text,data=render();target=Path(__file__).with_name('wire_golden.h')
 if args.write:target.write_bytes(text.encode())
 else:assert target.read_text()==text
 print(json.dumps(dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),schema=32,semantic=84,product_writer_used=False)))
