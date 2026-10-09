"""Independent complete models; no Xray writer, compiler or executable is read.

The new codec has one dense construction count after type tables and before
parameter defaults, recursively in every encoded Module. These models contain
no nominal declarations, so that mandatory count is exactly the u32 zero.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re,struct
def load(path,name):
 spec=importlib.util.spec_from_file_location(name,path)
 module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def literal(path,symbol):
 text=path.read_text('utf-8');m=re.search(r'\b'+symbol+r'\s*\[[^]]*\]\s*=\s*\{(.*?)\}',text,re.S)
 assert m,(path,symbol)
 body=re.sub(r'/\*.*?\*/|//[^\n]*','',m[1],flags=re.S)
 return bytes(int(v,0) for v in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b',body))
def frame(body,schema,semantic):
 prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',schema,semantic,2,0,len(body))
 return prefix+hashlib.sha256(prefix+body).digest()+body
def header(path,guard,rows,write,macros=()):
 def begin(name,own_guard):
  return ['/*',' * xray - Lightweight typed scripting with native concurrency',
   ' * https://www.xray-lang.org',' *',
   ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
   ' * Licensed under the MIT License',' *',
   ' * '+name+' - Independent complete construction packet fields',' *',
   ' * KEY CONCEPT:',
   ' *   Literal bytes are derived from fixed field models without a compiler writer.',
   ' */','#ifndef '+own_guard,'#define '+own_guard,'#include <stdint.h>']
 def array(name,data):
  return ['static const uint8_t '+name+'[] = {']+[
   '    '+', '.join('0x%02x'%b for b in data[i:i+12])+',' for i in range(0,len(data),12)]+['};']
 def emit(target,parts,own_guard):
  parts.append('#endif // '+own_guard);text='\n'.join(parts)+'\n'
  assert len(parts)<=800,(target,len(parts))
  if write:target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text,encoding='utf-8',newline='\n')
  else:assert target.read_text('utf-8')==text
 arrays=[array(name,data) for name,data in rows]
 parts=begin(path.name,guard)
 if len(parts)+sum(map(len,arrays))+len(macros)+1<=800:
  for item in arrays:parts.extend(item)
 else:
  # Keep whole independent vectors intact in bounded standalone include groups.
  chunks=[];chunk=[];size=0
  for item in arrays:
   assert len(item)+16<=800
   if chunk and size+len(item)>700:chunks.append(chunk);chunk=[];size=0
   chunk.extend(item);size+=len(item)
  if chunk:chunks.append(chunk)
  for index,chunk in enumerate(chunks,1):
   child=path.with_name(path.stem+'_part'+str(index)+'.h')
   child_guard=guard.removesuffix('_H')+'_PART'+str(index)+'_H'
   emit(child,begin(child.name,child_guard)+chunk,child_guard)
   parts.append('#include "'+child.name+'"')
 parts.extend('#define '+name+' '+str(value)+'u' for name,value in macros)
 emit(path,parts,guard)

def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True)
 p.add_argument('--output-root',type=Path,required=True);p.add_argument('--write',action='store_true');a=p.parse_args()
 src=a.source_root/'tests/unit/xir';out=a.output_root/'tests/unit/xir';records=[]
 c=load(src/'derive_callable71_checked_vectors.py','construction_callable_model')
 groups=[('8','xir_checked_scalar71_golden.h','checked_scalar71_golden',8),
  ('14','xir_generic_method71_golden.h','generic_method71_golden',8),
  ('14','xir_generic_method71_golden.h','generic_method71_invalid_flags0',0),
  ('14','xir_generic_method71_golden.h','generic_method71_invalid_flags1',1)]
 generated={}
 for key,file,name,upper in groups:
  model=c.MODELS[key];assert not model['nominals'] and not model['defaults']
  body,offsets=c.encode_body(model,upper)
  prior=frame(body,26,71);assert prior==literal(src/file,name)
  # These fully explicit models end in default-count0, provenance-ABSENT0.
  assert body[-8:]==bytes(8)
  body72=body[:-8]+struct.pack('<I',0)+body[-8:]
  current=frame(body72,27,72);newname=name.replace('71','72')
  generated[newname]=current
  records.append({'name':newname,'previous_sha256':hashlib.sha256(prior).hexdigest(),
   'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest(),'body_insertion':len(body)-8})
 header(out/'xir_checked_scalar72_golden.h','XIR_CHECKED_SCALAR72_GOLDEN_H',[
  ('checked_scalar72_golden',generated['checked_scalar72_golden']),('checked_scalar72_digest',generated['checked_scalar72_golden'][32:64])],a.write)
 header(out/'xir_generic_method72_golden.h','XIR_GENERIC_METHOD72_GOLDEN_H',[
  (k,v) for k,v in generated.items() if k.startswith('generic')],a.write,
  [('GENERIC_METHOD72_MAP_OWN',544),('GENERIC_METHOD72_COPY_OWN',584)])
 sizing=load(src/'compile_owner/derive_checked_sizing71_golden.py','construction_sizing_model');rows=[]
 for suffix,name in [('golden',b'main'),('embedded_golden',b'm\0in'),('empty_golden',b'')]:
  prior=sizing.fixed_program(name,26,71,33)
  assert prior==literal(src/'compile_owner/checked_sizing71_golden.h','sizing71_'+suffix)
  # Complete scalar model: operands0/generics0/types(0,0,0)/defaults0/evidence0.
  body=prior[64:];assert body[-28:]==bytes(28)
  current=frame(body[:-8]+bytes(4)+body[-8:],27,72);assert len(current)==224+len(name)
  rows.append(('sizing72_'+suffix,current));records.append({'name':rows[-1][0],
   'previous_sha256':hashlib.sha256(prior).hexdigest(),'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest()})
 header(out/'compile_owner/checked_sizing72_golden.h','CHECKED_SIZING72_GOLDEN_H',rows,a.write)
 panics=load(src/'derive_assert_panics71_vector.py','construction_panics_model')
 body,offsets=panics.core_body(panics.OPCODES,root_upper=8,template=True)
 prior=frame(body,26,71);assert prior==literal(src/'xir_assert_panics71_golden.h','assert_panics71_golden')
 position=offsets['defaults']-64
 current=frame(body[:position]+bytes(4)+body[position:],27,72)
 macros=[('XR_PANICS72_VECTOR_'+name.upper(),value+(4 if value>=offsets['defaults'] else 0)) for name,value in offsets.items()]
 macros.append(('XR_PANICS72_CALLABLE_FLAGS',1915))
 header(out/'xir_assert_panics72_golden.h','XIR_ASSERT_PANICS72_GOLDEN_H',[('assert_panics72_golden',current)],a.write,macros)
 records.append({'name':'assert_panics72_golden','previous_sha256':hashlib.sha256(prior).hexdigest(),
  'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest(),'construction_offset':offsets['defaults'],
  'defaults_offset':offsets['defaults']+4,'evidence_offset':offsets['evidence']+4})
 for stem,script,symbol,args,file,newfile,newname in [
  ('range','derive_range_checked70_goldens.py','checked_range70_golden',(70,),'xir_checked_range70_golden.h','xir_checked_range72_golden.h','checked_range72_golden'),
  ('rune','derive_rune_checked_golden.py','rune65_golden',(25,65,136,137,33),'xir_rune65_golden.h','xir_rune72_golden.h','rune72_golden')]:
  model=load(src/script,'construction_'+stem+'_model');prior=model.packet(*args)
  assert prior==literal(src/file,symbol)
  body=prior[64:];assert body[-8:]==bytes(8)
  current=frame(body[:-8]+bytes(4)+body[-8:],27,72)
  header(out/newfile,stem.upper()+'72_GOLDEN_H',[(newname,current)],a.write)
  records.append({'name':newname,'previous_sha256':hashlib.sha256(prior).hexdigest(),
   'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest(),'construction_offset':len(prior)-8})
 print(json.dumps({'status':'INDEPENDENT_MODELS_DERIVED_NO_C_RUN','wire':27,'semantic':72,
  'complete_historical_models_reproduced':len(records),'vectors':records,
  'production_writer_used':False,'recursive_nonzero_nominal_vectors':'SEPARATE_REQUIRED_GATE'},indent=2))
if __name__=='__main__':main()
