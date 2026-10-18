"""Independent full Generator factory packets from named fields and fixed ordinals."""
from pathlib import Path
import argparse,hashlib,json,struct
from derive_construction72_vectors import header

def words(*v):return struct.pack('<'+'I'*len(v),*v)
def blob(v):
 b=v.encode();return words(len(b))+b
def instruction(op,result=0,left=0,right=0,immediate=0,targets=(0,0)):
 return words(op,result,left,right,*targets)+struct.pack('<q',immediate)+words(0,0)
def function(name,result,code,operands=(),parameters=()):
 return blob(name)+words(len(parameters),*parameters,result,1,0,len(code),0,0,len(code))+b''.join(code)+words(len(operands),*operands)
def packet(variant):
 if variant in ("owned_cell","borrowed_cell"):return cell_packet(variant=="borrowed_cell")
 unit=variant=='unit';element=0 if unit else 2
 generator=[instruction(176)] if unit else [instruction(2,2,immediate=11),instruction(176,left=0)]
 generator+=[instruction(33)]
 many=variant=='argument_count'
 factory=([instruction(2,2,immediate=7)] if many else [])
 factory+=[instruction(176 if variant=='yield_in_factory' else 177,
  2 if variant=='scalar_result' else 256,right=int(many),
  immediate=2 if variant=='ordinary_target' else 1,targets=(1,0) if variant=='targets' else (0,0)),instruction(33,left=int(many))]
 body=words(0,4,1)+function('init',0,[instruction(33)])
 body+=function('numbers',256,generator)+function('make',256,factory,(0,) if many else ())
 body+=function('entry',2,[instruction(2,2),instruction(33)])
 body+=words(1,0,0,0,3)+blob('factory')+words(0,0)
 body+=words(0,0,0,0,0,0,0,0,0)+words(0,0,0,0,0,2,0,0,0)
 body+=words(0,1,0,0,0,1 if variant=='no_suspend' else 0,0,0,0)+words(0,0,0,0,0,0,0,0,0)+words(0)
 body+=words(0,1,0,0,11,0,element)+words(0,0,0)
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,89,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def cell_packet(borrowed):
 producer=[instruction(60,2,left=0),instruction(176,left=1),instruction(33)]
 make=([instruction(177,256,right=1,immediate=1),instruction(33,left=1)] if borrowed else
       [instruction(2,2,immediate=7),instruction(59,257,left=0),instruction(177,256,right=1,immediate=1),instruction(33,left=2)])
 entry=([instruction(2,2,immediate=7),instruction(59,257,left=0),instruction(28,256,right=1,immediate=2),instruction(2,2),instruction(33,left=3)] if borrowed else
        [instruction(2,2),instruction(33)])
 body=words(0,4,1)+function('init',0,[instruction(33)])
 body+=function('numbers',256,producer,parameters=(257,))
 body+=function('make',256,make,(0,) if borrowed else (1,),parameters=(257,) if borrowed else ())
 body+=function('entry',2,entry,(1,) if borrowed else ())
 body+=words(1,0,0,0,3)+blob('factory')+words(0,0)
 body+=words(0,0,0,0,0,0,0,0,0)+words(0,0,0,0,0,2,0,0,0)
 body+=words(0,1,0,0,0,0,0,0,0)+words(0,0,0,0,0,0,0,0,0)+words(0)
 body+=words(0,2,0,0,11,0,2)+words(3,0,2)+words(0,0,0)
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,89,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--write',action='store_true');args=p.parse_args()
 names=('i64','unit','no_suspend','ordinary_target','scalar_result','argument_count','targets','yield_in_factory','owned_cell','borrowed_cell')
 rows=[('factory89_'+n,packet(n)) for n in names]
 header(Path(__file__).with_name('xir_generator_factory89_golden.h'),'XIR_GENERATOR_FACTORY89_GOLDEN_H',rows,args.write)
 print(json.dumps({'product_writer_used':False,'packets':[{'name':n,'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in rows]},indent=2))
if __name__=='__main__':main()
