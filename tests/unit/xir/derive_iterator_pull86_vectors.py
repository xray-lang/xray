"""Independent complete pull CFG packets with explicit ordinals and field models."""
from pathlib import Path
import argparse,hashlib,json,struct
from derive_construction72_vectors import header

def words(*xs):return struct.pack('<'+'I'*len(xs),*xs)
def text(s):
 b=s.encode();return words(len(b))+b
def ins(op,result=0,arg=0,normal=0,error=0,origin=0):
 return words(op,result,arg,0,normal,error)+struct.pack('<q',origin)+words(0,0)
def packet(name):
 unit=name in ('next_unit','has_next_unit')
 has=name.startswith('has_next')
 result=1 if has else 0 if unit else 2
 parameter=2 if name=='wrong_receiver' else 256
 normal=[] if result==0 else [ins(101,1 if name=='wrong_result' else result,origin=0)]
 normal+=[ins(33,arg=0 if result==0 else 2)]
 error_first=1+len(normal)
 code=[ins(178 if has else 179,normal=1,error=1 if name=='same_successor' else 2)]
 code+=normal+[ins(102,14,origin=1 if name=='wrong_origin' else 0),ins(30,arg=1+error_first)]
 if name=='unused_immediate':code[0]=ins(179,normal=1,error=2,origin=1)
 body=words(0,1,0)+text('pull')+words(1,parameter,result,3)
 body+=words(0,1,0,0,1,len(normal),0,0,error_first,2,0,0,len(code))+b''.join(code)+words(0)
 body+=words(0,1,0,0,11,0,0 if unit else 2,0,0,0)
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,86,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--write',action='store_true');a=p.parse_args()
 names=('has_next_i64','next_i64','has_next_unit','next_unit','wrong_receiver','wrong_result','same_successor','wrong_origin','unused_immediate')
 rows=[('pull86_'+n,packet(n)) for n in names]
 header(Path(__file__).with_name('xir_iterator_pull86_golden.h'),'XIR_ITERATOR_PULL86_GOLDEN_H',rows,a.write)
 print(json.dumps({'product_writer_used':False,'packets':[{'name':n,'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in rows]},indent=2))
if __name__=='__main__':main()
