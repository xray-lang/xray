"""Derive real Checked29/74 peers from complete independent historical fields.

Historical bytes remain immutable. Current declarations gain a function binding
index, an implementation policy index and two actual derivation table counts.
The nonempty grant model below encodes all fields independently of the product.
Historical outputs use their exact wire/semantic namespace because other layouts
retained the same earlier filenames. No product reader accepts those old bytes.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re,struct
from derive_derivation74_fields import Fields,inventory

def load(path,name):
 spec=importlib.util.spec_from_file_location(name,path)
 module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
def words(*values):return struct.pack('<'+'I'*len(values),*values)
def frame(payload,wire,semantic):
 header=b'XRCHK\0\0\0'+words(wire,semantic,2,0)+struct.pack('<Q',len(payload))
 return header+hashlib.sha256(header+payload).digest()+payload
def origin(line):
 def blob(text):data=text.encode('utf-8');return words(len(data))+data
 return blob('alpha')+blob('alpha/record.xr')+blob('Record.secret')+words(line,7)
def ordinary_grant(encoder):
 def op(code,result=0,left=0,right=0,immediate=0):return [code,result,left,right,0,0,immediate,0,0]
 def function(name,result,ops,parameters=()):
  return dict(name=name,parameters=list(parameters),result=result,blocks=[[0,len(ops),0,0]],instructions=ops,operands=[])
 model=dict(linkage=0,functions=[
  function('main',2,[op(2,2,immediate=41),op(33)]),function('init',0,[op(33)]),
  function('measure',2,[op(83,2),op(33,left=1)],[256]),
  function('generator',2,[op(2,2,immediate=41),op(33)])],
  declarations=dict(modules=[['alpha',[],1]],identities=[
   [0,1,0,0,0,0,0,0,0],[0]*9,[0,1,1,0,0,0,1,0,0],[0,1,0,0,0,0,0,0,0]],
   slots=[],literals=[],root=0,entry=0),generics=None,
  nodes=[[4,0,0,[],[2]],[1,0,[],2,8]],
  nominals=[['alpha','Record',1,0,0,0,'00'*32,[],[['secret',2,5]],[]]],
  interfaces=[['alpha',name,1,[],[],[['measure',257,0,[]]]] for name in ('Measure','Decode')],defaults=[])
 body,_=encoder.encode_body(model,8)
 assert body[-8:]==bytes(8)
 # One nominal, no default constructor, one explicit zero field initializer.
 legacy=frame(body[:-8]+words(1,0,1,0)+body[-8:],28,73)
 fields=Fields(legacy);fields.upgrade();assert len(fields.insertions)==5
 edits=[]
 for i,(at,data) in enumerate(fields.insertions[:-1]):edits.append((at,at,words(int(i==2))))
 at=fields.insertions[-1][0];assert legacy[at-4:at]==bytes(4)
 implementation=words(1,0,1,0,0,1,0,0,0,2)
 edits.append((at-4,at,implementation))
 policy=words(0,256,0,0,3,0,1,0,3)+origin(23)
 binding=words(2,0,0,0,256)+origin(23)+words(2)+origin(23)+origin(24)
 table=words(1)+policy+words(1)+binding;edits.append((at,at,table))
 output=bytearray();last=64
 for start,end,data in sorted(edits):
  assert last<=start<=end;output+=legacy[last:start]+data;last=end
 output+=legacy[last:]
 return frame(output,29,74),model

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[3]);parser.add_argument('--output-root',type=Path);parser.add_argument('--write',action='store_true');args=parser.parse_args()
 root=args.source_root;src=root/'tests/unit/xir';out=(args.output_root or root)/'tests/unit/xir/historical_packets/checked29_74'
 writer=load(src/'derive_construction72_vectors.py','derivation_header_output')
 records,errors=inventory(root);assert len(records)==13 and not errors,errors
 groups={};report=[]
 for row in records:groups.setdefault(row['path'],[]).append(row)
 for path,rows in groups.items():
  old=root/path;name=old.name.replace('73','74') if '73' in old.name else old.name.replace('72','74')
  current=[];macros=[]
  for row in rows:
   symbol=row['name'].replace('73','74') if '73' in row['name'] else row['name'].replace('72','74')
   current.append((symbol,bytes.fromhex(row['current_hex'])))
  traversal=Fields(writer.literal(old,rows[0]['name']));traversal.upgrade()
  for m in re.finditer(r'^#define\s+(\w+)\s+(\d+)u?\s*$',old.read_text(encoding='utf-8-sig'),re.M):
   macro=m[1].replace('73','74') if '73' in m[1] else m[1].replace('72','74')
   if 'RECURSIVE_OWNER' in macro:
    row=next(row for row in rows if row['name'].endswith('_instance'))
    traversal=Fields(writer.literal(old,row['name']));traversal.upgrade()
   macros.append((macro,traversal.mapped(int(m[2]))))
  if 'scalar73' in old.name:current.append(('checked_scalar74_digest',current[0][1][32:64]))
  target=out/old.relative_to(src).parent/name
  writer.header(target,name.upper().replace('.','_'),current,args.write,macros)
  report.append(dict(path=target.relative_to(out).as_posix(),names=[symbol for symbol,_ in current],macros=macros))
 history=[('grant74_previous_'+str(i),writer.literal(root/row['path'],row['name'])) for i,row in enumerate(records)]
 writer.header(out/'grant74_historical_goldens.h','GRANT74_HISTORICAL_GOLDENS_H',history,args.write)
 encoder=load(src/'derive_callable71_checked_vectors.py','derivation_ordinary_fields')
 current,model=ordinary_grant(encoder)
 writer.header(out/'xir_derivation74_golden.h','XIR_DERIVATION74_GOLDEN_H',[('derivation74_golden',current)],args.write)
 print(json.dumps(dict(status='INDEPENDENT_COMPLETE_FIELDS_ONLY',historical=records,outputs=report,nonempty_model=model,nonempty_bytes=len(current),nonempty_sha256=hashlib.sha256(current).hexdigest(),product_writer_used=False,executable_read=False),indent=2))
if __name__=='__main__':main()
