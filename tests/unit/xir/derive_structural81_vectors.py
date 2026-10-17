"""Independent complete structural contracts, definition binding and malformed controls."""
from pathlib import Path
import argparse,hashlib,struct,json
from derive_nested_nullable_vectors import words,operation,function
from derive_record80_vectors import declarations,record
from derive_semantic80_vectors import Cursor as CompleteCursor
from derive_construction72_vectors import header
ROOT=Path(__file__).resolve().parent
class Cursor(CompleteCursor):
    def constraints(self,count):
        for _ in range(count):
            self.word();self.applications();self.vector(8)
def packet(policy=1,selection=0,result=2,shape=257,required=None,extra=(),used=True,requirements=None,field_result=None):
    init=function('$init',[],0,[operation(33)])
    call=operation(28,2,left=2,right=1,immediate=2)[:-8]+words(0,1)
    entry=(function('referenceAnswer',[],2,[operation(1,1,immediate=1),operation(2,2,immediate=7),
        operation(154,256,right=2),call,operation(33,left=3)],[0,1,2]) if used else
        function('referenceAnswer',[],2,[operation(2,2,immediate=7),operation(33,left=0)]))
    field_result=result if field_result is None else field_result
    body=[operation(157,field_result,immediate=selection)]
    if field_result!=result:
        assert result==2;body+=[operation(2,2,immediate=7),operation(33,left=2)]
    else:body+=[operation(33,left=1)]
    read=function('read',[65536],result,body)
    contracts=[(shape,policy)] if requirements is None else requirements
    generics=words(0,0,0)+(words(0,0,1,256) if used else words(0,0,0))
    generics+=words(1,0,0,0,len(contracts))+b''.join(words(*r) for r in contracts)+words(0)
    nodes=[record([('a',1),('n',2)]),record(required if required is not None else [('n',2)])]+list(extra)
    body=words(0,3,1)+init+entry+read+declarations(3)+words(1)+generics
    body+=words(len(nodes),0,0)+b''.join(nodes)+words(0,0,0)
    cursor=Cursor(body,32);cursor.module();assert cursor.at==len(body) and bytes(cursor.out)==body
    prefix=b'XRCHK\0\0\0'+words(33,81,2,0)+struct.pack('<Q',len(body))
    assert len(prefix)==32
    return prefix+hashlib.sha256(prefix+body).digest()+body
def models():
    return [('structural81_relocated',packet()),('structural81_policy_zero',packet(policy=0)),
        ('structural81_policy_unknown',packet(policy=2)),
        ('structural81_tuple_shape',packet(shape=258,extra=[words(6,0,1,2)])),
        ('structural81_extra_selection',packet(selection=1)),
        ('structural81_result_mismatch',packet(result=1,used=False)),
        ('structural81_missing',packet(required=[('z',2)])),
        ('structural81_field_mismatch',packet(required=[('n',1)],field_result=1)),
        ('structural81_unused_policy',packet(policy=0,used=False)),
        ('structural81_unused_extra',packet(selection=1,used=False)),
        ('structural81_outside_binder',packet(shape=258,extra=[record([('n',65537)],2)])),
        ('structural81_conflict',packet(used=False,requirements=[(257,1),(258,1)],extra=[record([('n',1)])]))]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    rows=models();assert len(rows)==12
    header(ROOT/'xir_structural81_golden.h','XIR_STRUCTURAL81_GOLDEN_H',rows,args.write)
    print(json.dumps(dict(independent_complete_packets=len(rows),product_writer_used=False,
        packets=[dict(name=n,length=len(b),sha256=hashlib.sha256(b).hexdigest()) for n,b in rows])))
if __name__=='__main__':main()
