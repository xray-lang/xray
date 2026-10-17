"""Independent complete Generator packets from explicit field and ordinal models."""
from pathlib import Path
import argparse,hashlib,json,struct
from derive_construction72_vectors import header

def words(*values):return struct.pack('<'+'I'*len(values),*values)
def blob(value):
 data=value.encode('utf-8');return words(len(data))+data
def instruction(op,result=0,left=0,immediate=0):
 return words(op,result,left,0,0,0)+struct.pack('<q',immediate)+words(0,0)
def function(name,result,code,parameters=()):
 return blob(name)+words(len(parameters),*parameters,result,1,0,len(code),0,0,len(code))+b''.join(code)+words(0)
def packet(variant='i64'):
 unit=variant in ('unit','unit_arg')
 flags=0 if variant=='no_role' else 3 if variant=='no_suspend' else 0xffffffff if variant=='unknown_role' else 2
 element=0 if unit else 2
 parameters=(257,) if variant=='ref_parameter' else ()
 first=len(parameters)
 code=([instruction(176,left=int(variant=='unit_arg')),instruction(176)] if unit else
       [instruction(1 if variant=='wrong_value' else 2,1 if variant=='wrong_value' else 2,immediate=1 if variant=='wrong_value' else 11),instruction(176,left=first)])
 code+=[instruction(33,left=int(variant=='return_arg'))]
 body=words(0,3,1)+function('init',0,[instruction(33)])
 body+=function('units' if unit else 'numbers',2 if variant=='scalar_result' else 256,code,parameters)
 body+=function('entry',2,[instruction(2,2),instruction(33)])
 body+=words(1,0,0,0,2)+blob('generator')+words(0,0)
 body+=words(0,0,0,0,0,2 if variant=='initializer_role' else 0,0,0,0)
 body+=words(0,1,0,0,0,flags,0,0,0)+words(0,0,0,0,0,2 if variant=='entry_role' else 0,0,0,0)+words(0)
 kind=9 if variant=='reserved_map' else 10 if variant=='reserved_set' else 11
 body+=words(0,2 if parameters else 1,0,0,kind,0,element)
 if parameters:body+=words(3,0,2)
 body+=words(0,0,0)
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,89,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
 names=('i64','unit','no_role','scalar_result','wrong_value','return_arg','no_suspend','ref_parameter','unit_arg','unknown_role','initializer_role','entry_role','reserved_map','reserved_set')
 from derive_generator84_vectors import packet as previous84_packet
 previous84_names=names
 header(Path(__file__).with_name('xir_generator84_golden.h'),'XIR_GENERATOR84_GOLDEN_H',[('generator84_'+name,previous84_packet(name)) for name in previous84_names],False)
 from derive_generator74_vectors import packet as previous_packet
 previous_names=names[:-2]
 header(Path(__file__).with_name('xir_generator74_golden.h'),'XIR_GENERATOR74_GOLDEN_H',[('generator74_'+name,previous_packet(name)) for name in previous_names],False)
 rows=[('generator89_'+name,packet(name)) for name in names]
 header(Path(__file__).with_name('xir_generator89_golden.h'),'XIR_GENERATOR89_GOLDEN_H',rows,args.write)
 print(json.dumps({'writer_used':False,'named_field_models':True,'packets':[{'name':n,'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()} for n,b in rows]},indent=2))
if __name__=='__main__':main()
