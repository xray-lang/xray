"""Independently traverse complete Checked28/73 fields and insert real new fields.

This utility does not read an executable, product writer output or compiler code.
Historical fixed packets are inputs, and their complete digest and field extents
are checked before the exact zero grant data is introduced at each declaration.
"""
from pathlib import Path
import hashlib,json,re,struct

class Fields:
 def __init__(self,data):self.data=data;self.p=64;self.insertions=[];self.modules=0
 def skip(self,n):
  assert 0<=n<=len(self.data)-self.p,(self.p,n,len(self.data))
  self.p+=n
 def u32(self):
  assert self.p+4<=len(self.data)
  n=struct.unpack_from('<I',self.data,self.p)[0];self.p+=4;return n
 def vector(self,width):
  n=self.u32();assert n<=1048576;self.skip(n*width);return n
 def blob(self):return self.vector(1)
 def app(self):self.u32();self.vector(4)
 def apps(self):
  for _ in range(self.u32()):self.app()
 def constraints(self,n):
  for _ in range(n):self.u32();self.apps()
 def insert(self,n):self.insertions.append((self.p,bytes(n)))
 def module(self):
  self.modules+=1;assert self.modules<4
  linkage,functions,declarations=self.u32(),self.u32(),self.u32()
  assert linkage in (0,1) and 0<functions<=65536 and declarations in (0,1)
  for _ in range(functions):
   self.blob();self.vector(4);self.u32();self.vector(16);self.vector(40);self.vector(4)
  if declarations:
   modules,slots,literals=self.u32(),self.u32(),self.u32();self.skip(8)
   for _ in range(modules):self.blob();self.vector(4);self.u32()
   for _ in range(functions):self.skip(36);self.insert(4)
   self.skip(slots*12)
   for _ in range(literals):self.blob()
   for _ in range(self.u32()):
    self.u32();self.insert(4);self.app()
    for _ in range(self.u32()):self.app();self.skip(8)
   self.insert(8)
  generics=self.u32();assert generics in (0,1)
  if generics:
   for _ in range(functions):
    count,kinds=self.u32(),self.u32();assert kinds in (0,1)
    if kinds:self.skip(count*4)
    self.constraints(count);self.vector(4)
  nodes,nominals,interfaces=self.u32(),self.u32(),self.u32()
  for _ in range(nodes):
   kind=self.u32();self.u32()
   if kind==1:self.vector(8);self.skip(8)
   elif kind in (2,3,5,7,8):self.u32()
   elif kind==6:self.vector(4)
   elif kind==4:self.u32();self.vector(4);self.vector(4)
   else:raise AssertionError(('kind',kind,self.p))
  for _ in range(nominals):
   self.blob();self.blob();self.skip(48);self.constraints(self.u32())
   for _ in range(self.u32()):self.blob();self.skip(8)
   for _ in range(self.u32()):self.blob();self.skip(8)
  for _ in range(interfaces):
   self.blob();self.blob();self.u32();self.constraints(self.u32());self.apps()
   for _ in range(self.u32()):self.blob();self.skip(8);self.constraints(self.u32())
  construction=self.u32();assert construction==nominals,(construction,nominals,self.p)
  for _ in range(construction):self.u32();self.vector(4)
  self.vector(16)
  provenance=self.u32();assert provenance in (0,1,2)
  if provenance==1:
   count=self.u32();assert count==functions
   for _ in range(count):self.vector(8);self.u32();self.vector(8);self.vector(12);self.vector(16)
  if provenance==2:
   self.module();count=self.u32();assert count==functions
   for _ in range(count):self.u32();self.vector(4);self.vector(8)
   self.vector(24)
 def upgrade(self):
  assert self.data[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',self.data,8)==(28,73)
  assert struct.unpack_from('<Q',self.data,24)[0]==len(self.data)-64
  assert hashlib.sha256(self.data[:32]+self.data[64:]).digest()==self.data[32:64]
  self.module();assert self.p==len(self.data),(self.p,len(self.data))
  payload=bytearray();last=64
  for at,added in self.insertions:
   assert at>=last;payload+=self.data[last:at]+added;last=at
  payload+=self.data[last:];header=bytearray(self.data[:32])
  struct.pack_into('<II',header,8,29,74);struct.pack_into('<Q',header,24,len(payload))
  return bytes(header)+hashlib.sha256(header+payload).digest()+payload
 def mapped(self,old):return old+sum(len(data) for at,data in self.insertions if at<=old)

def inventory(root):
 expression=re.compile(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',re.S)
 records=[];errors=[]
 paths=['compile_owner/checked_sizing73_golden.h','root_construction73_goldens.h',
        'xir_assert_panics73_golden.h','xir_checked_range73_golden.h',
        'xir_checked_scalar73_golden.h','xir_error_marker73_golden.h',
        'xir_generic_method73_golden.h','xir_rune73_golden.h']
 for path in sorted(root/'tests/unit/xir'/name for name in paths):
  for m in expression.finditer(path.read_text(encoding='utf-8-sig')):
   literal=re.sub(r'/\*.*?\*/|//[^\n]*','',m[2],flags=re.S)
   values=re.findall(r'0x[0-9a-fA-F]+|\b\d+\b',literal)
   if not values or any(int(v,0)>255 for v in values):continue
   data=bytes(int(v,0) for v in values)
   if len(data)<64 or data[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',data,8)!=(28,73):continue
   try:
    fields=Fields(data);current=fields.upgrade()
    records.append(dict(path=path.relative_to(root).as_posix(),name=m[1],old_sha256=hashlib.sha256(data).hexdigest(),old_bytes=len(data),new_sha256=hashlib.sha256(current).hexdigest(),new_bytes=len(current),modules=fields.modules,insertions=[dict(at=at,bytes=len(blob)) for at,blob in fields.insertions],current_hex=current.hex()))
   except (AssertionError,struct.error) as e:errors.append(dict(path=str(path),name=m[1],reason=str(e)))
 return records,errors

if __name__=='__main__':
 root=Path.cwd();records,errors=inventory(root)
 out=root/'build/p12-description-logs/grant74-vector-inventory.json'
 out.write_text(json.dumps(dict(records=records,errors=errors),indent=2)+'\n',encoding='utf-8')
 print(json.dumps(dict(records=len(records),files=len({r['path'] for r in records}),errors=errors)))
