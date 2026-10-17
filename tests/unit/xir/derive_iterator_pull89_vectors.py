"""Independent complete pull CFG packets with explicit ordinals and field models."""
from pathlib import Path
import argparse,hashlib,json,struct
from derive_construction72_vectors import header

def words(*xs):return struct.pack('<'+'I'*len(xs),*xs)
def text(s):
 b=s.encode();return words(len(b))+b
def ins(op,result=0,arg=0,normal=0,error=0,origin=0,right=0):
 return words(op,result,arg,right,normal,error)+struct.pack('<q',origin)+words(0,0)
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
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,89,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def nth_packet(name):
 unit=name=='nth_unit';result=0 if unit else 2
 parameter=2 if name=='nth_wrong_receiver' else 256
 index_type=1 if name=='nth_wrong_index_type' else 2
 normal=[] if unit else [ins(101,1 if name=='nth_wrong_result' else result,origin=1 if name=='nth_wrong_origin' else 0)]
 normal+=[ins(33,arg=0 if unit else 3)]
 error_first=1+len(normal)
 code=[ins(180,arg=0,right=2 if name=='nth_wrong_index_id' else 1,normal=1,
           error=1 if name=='nth_same_successor' else 2,origin=1 if name=='nth_unused_immediate' else 0)]
 code+=normal+[ins(102,14),ins(30,arg=2+error_first)]
 body=words(0,1,0)+text('nth')+words(2,parameter,index_type,result,3)
 body+=words(0,1,0,0,1,len(normal),0,0,error_first,2,0,0,len(code))+b''.join(code)+words(0)
 body+=words(0,1,0,0,11,0,0 if unit else 2,0,0,0)
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,89,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def close_packet(name):
 unit=name=='close_unit';parameter=2 if name=='close_wrong_receiver' else 256
 normal=[ins(101,2)] if name=='close_result_payload' else []
 normal+=[ins(33)]
 error_first=1+len(normal)
 code=[ins(181,1 if name=='close_wrong_result' else 0,right=1 if name=='close_unused_right' else 0,
           normal=1,error=1 if name=='close_same_successor' else 2,origin=1 if name=='close_unused_immediate' else 0)]
 code+=normal+[ins(102,14,origin=1 if name=='close_wrong_origin' else 0),ins(30,arg=1+error_first)]
 body=words(0,1,0)+text('close')+words(1,parameter,0,3)
 body+=words(0,1,0,0,1,len(normal),0,0,error_first,2,0,0,len(code))+b''.join(code)+words(0)
 body+=words(0,1,0,0,11,0,0 if unit else 2,0,0,0)
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,89,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--write',action='store_true');a=p.parse_args()
 names=('has_next_i64','next_i64','has_next_unit','next_unit','wrong_receiver','wrong_result','same_successor','wrong_origin','unused_immediate')
 rows=[('pull89_'+n,packet(n)) for n in names]
 nth_names=('nth_i64','nth_unit','nth_wrong_receiver','nth_wrong_index_type','nth_wrong_index_id','nth_wrong_result','nth_same_successor','nth_wrong_origin','nth_unused_immediate')
 rows += [('pull89_'+n,nth_packet(n)) for n in nth_names]
 close_names=('close_i64','close_unit','close_wrong_receiver','close_result_payload','close_same_successor','close_wrong_origin','close_unused_immediate','close_unused_right','close_wrong_result')
 rows += [('pull89_'+n,close_packet(n)) for n in close_names]
 header(Path(__file__).with_name('xir_iterator_pull89_golden.h'),'XIR_ITERATOR_PULL89_GOLDEN_H',rows,a.write)
 print(json.dumps({'product_writer_used':False,'packets':[{'name':n,'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in rows]},indent=2))
if __name__=='__main__':main()
