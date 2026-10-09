"""Full named fields for retained Task/Tuple/Nullable/Atomic/assertion gates.
Historical literals are comparisons only. Never read a runtime emitted packet.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct,sys

def load(p,name):
 s=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True);p.add_argument('--output-root',type=Path,required=True);p.add_argument('--current-root',type=Path);p.add_argument('--write',action='store_true');a=p.parse_args()
 src=a.source_root/'tests/unit/xir';out=a.output_root/'tests/unit/xir';sys.path.insert(0,str(src))
 current_path=(a.current_root or a.source_root)/'tests/unit/xir'
 utility=load(current_path/'derive_construction72_vectors.py','family_util');words=lambda *v:struct.pack('<'+'I'*len(v),*v)
 frame=utility.frame;records=[];groups={}
 def add(file,name,body,oldschema,oldsem,oldfile,oldname,offset,facts):
  prior=frame(body,oldschema,oldsem);assert prior==utility.literal(src/oldfile,oldname),(name,'whole prior field model')
  current=frame(body[:offset]+facts+body[offset:],27,72)
  groups.setdefault(file,[]).append((name,current))
  records.append({'name':name,'prior_sha256':hashlib.sha256(prior).hexdigest(),'prior_bytes':len(prior),'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest(),'construction_offset':64+offset,'fact_bytes':len(facts)})
 task=load(src/'derive_task_checked70_goldens.py','family_task')
 for kind,entries in [('types',[('golden',task.types_body(2))]),('unit',[('golden',task.types_body(0))]),('go',[(role,task.go_body(role)) for role in task.ROLE_NAMES])]:
  for role,body in entries:
   add('xir_task_'+kind+'72_golden.h','task_'+kind+'72_'+role,body,25,70,'xir_task_'+kind+'70_golden.h','task_'+kind+'70_'+role,len(body)-8,words(0))
 nested=load(src/'derive_nested_nullable_vectors.py','family_nested')
 body=nested.nested_packet(65)[64:]
 add('xir_nested_nullable72_golden.h','nested_nullable72_golden',body,25,65,'xir_nested_nullable65_golden.h','nested_nullable65_golden',len(body)-8,words(0))
 # Direct Tuple value73, Unit/String/i64 fields, explicit operands(1,0).
 def op(code,ty=0,left=0,right=0,imm=0):return words(code,ty,left,right,0,0)+struct.pack('<q',imm)+words(0,0)
 def counted(b):return words(len(b))+b
 def fn(name,params,result,ops,operands=()):return counted(name)+words(len(params),*params,result,1,0,len(ops),0,0,len(ops))+b''.join(ops)+words(len(operands),*operands)
 ops=[op(2,2,imm=73),op(143,256,right=2),op(144,0,left=2),op(144,3,left=2,imm=2),op(144,2,left=2,imm=1),op(33,left=5)]
 body=words(0,1,0)+fn(b'tuple',[3],2,ops,[1,0])+words(0,1,0,0,6,0,3,0,2,3,0,0)
 add('xir_tuple72_golden.h','tuple72_golden',body,25,65,'tuple_25_65.inc.c','tuple_25_65',len(body)-8,words(0))
 # Atomic<T> signature has exactly three physical descriptors and no generics.
 body=words(0,1,0)+fn(b'types',[256,257,258],0,[op(33)])+words(0,3,0,0,7,0,2,7,0,1,7,0,13,0,0)
 add('xir_atomic_types72_golden.h','atomic_types72_golden',body,25,65,'xir_atomic_types65_golden.h','atomic_types65_golden',len(body)-8,words(0))
 # Governed Ordering enum has one nominal declaration and zero fields.
 module=b'stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def'
 body=words(0,3,1)+fn(b'root_init',[],0,[op(33)])+fn(b'ordering_init',[],0,[op(33)])+fn(b'main',[],2,[op(2,2,imm=7),op(33)])
 body+=words(2,0,0,0,2)+counted(b'root')+words(1,1,0)+counted(module)+words(0,1)
 body+=words(0,0,0,0,0,0,0,0,0)+words(1,0,0,0,0,0,0,0,0)+words(0,1,0,0,0,0,0,0,0)
 body+=words(0,0,1,1,0,4,0,0,0,0)+counted(module)+counted(b'Ordering')+words(1,1,0,4)
 body+=bytes.fromhex('7c7e1749ecae49adc12a579614991f34236c02f61497b439cb61073f9893d18c')+words(0,0,5)
 for name in (b'Relaxed',b'Acquire',b'Release',b'AcquireRelease',b'SeqCst'):body+=counted(name)+words(0,0)
 body+=words(0,0)
 add('xir_ordering72_golden.h','ordering72_golden',body,25,65,'xir_ordering65_golden.h','ordering65_golden',len(body)-8,words(1,0,0))
 # Standalone condition assertion projects three functions, one empty literal,
 # one default binding; complete Core equality instead shares all seven functions.
 identity=b'memory-module-v1:id=23:xray-core-assertions-v1'
 body=words(1,3,1)+fn(b'$init',[],0,[op(33)])+fn(b'assert',[1,3],0,[op(123,right=1),op(33)])+fn(b'$argument_default',[],3,[op(3,3),op(33)])
 body+=words(1,0,1,0xffffffff,0xffffffff)+counted(identity)+words(0,0)
 for exported in (0,1,0):body+=words(0,exported,0,0,0,0,0,0,0)
 body+=words(0,0)+words(0,0,0,0)+words(1,0,1,1,2)+words(0)
 add('xir_assert_condition72_golden.h','assert_condition72_golden',body,25,65,'xir_assert_condition65_golden.h','assert_condition65_golden',len(body)-24,words(0))
 core=load(src/'derive_assert_panics71_vector.py','family_equal')
 body,offsets=core.core_body(core.OPCODES,root_upper=0,template=False)
 assert frame(body,25,65)==utility.literal(src/'xir_assert_equal65_golden.h','assert_equal65_golden')
 current_body,new_offsets=core.core_body(core.OPCODES,root_upper=8,template=True)
 position=new_offsets['defaults']-64;current=frame(current_body[:position]+words(0)+current_body[position:],27,72)
 assert current==utility.literal(current_path/'xir_assert_panics72_golden.h','assert_panics72_golden')
 records.append({'name':'assertEqual shares exact complete Core72 oracle','prior_bytes':len(body)+64,'bytes':len(current),'prior_sha256':hashlib.sha256(frame(body,25,65)).hexdigest(),'sha256':hashlib.sha256(current).hexdigest(),'construction_offset':position+64})
 for file,rows in groups.items():utility.header(out/file,file.upper().replace('.','_'),rows,a.write)
 print(json.dumps({'status':'INDEPENDENT_FULL_FIELDS_NOT_C_EXECUTED','models':records,'production_writer_used':False},indent=2))
if __name__=='__main__':main()
