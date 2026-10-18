"""Encode every populated generic call-binding field without a compiler writer."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, struct

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True);parser.add_argument('--output-root',type=Path,required=True)
    parser.add_argument('--write',action='store_true');args=parser.parse_args()
    base=args.source_root/'tests/unit/xir';utility=load(base/'derive_construction72_vectors.py','binding_fields')
    encoder=load(base/'derive_callable71_checked_vectors.py','binding_module_fields')
    words=lambda *values:struct.pack('<'+'I'*len(values),*values)
    def op(code,result=0,left=0,right=0,immediate=0,offset=0,count=0):return [code,result,left,right,0,0,immediate,offset,count]
    def fn(name,result,body):return dict(name=name,parameters=[],result=result,blocks=[[0,len(body),0,0]],instructions=body,operands=[])
    model=dict(linkage=0,functions=[fn('init',0,[op(33)]),
        fn('main',2,[op(28,2,immediate=2,count=1),op(28,2,immediate=2,offset=1,count=1),op(33)]),
        fn('make',2,[op(28,2,immediate=3,count=1),op(33)]),fn('helper',2,[op(2,2,immediate=42),op(33)]),
        fn('constant',2,[op(2,2,immediate=42),op(33)])],
        declarations=dict(modules=[['alpha',[],0]],slots=[],literals=[],root=0,entry=1,
            identities=[[0,0,0,0,0,0,0,0,0],[0,1,0,0,0,0,0,0,0],*[ [0]*9 for _ in range(3)]]),
        generics=[[None,[],[]],[None,[],[5,3]],[None,[[0,[]]],[65536]],[None,[[0,[]]],[]],[None,[],[]]],
        nodes=[],nominals=[],interfaces=[],defaults=[])
    body,_=encoder.encode_body(model,8);assert body[-8:]==bytes(8)
    dense_and_materialization=words(0,0)
    sites=words(1,2,0,3)
    bindings=words(2,0,1,3,4,0,1,5,4)
    template=words(1,5)+b''.join(words(0,4 if f==0 else 0,0,0,0) for f in range(5))
    position=len(body)-8+len(dense_and_materialization)
    current=utility.frame(body[:-8]+dense_and_materialization+sites+bindings+words(0)+template,30,75)
    utility.header(args.output_root/'tests/unit/xir/comptime_binding75_golden.h','COMPTIME_BINDING75_GOLDEN_H',
        [('comptime_binding75_golden',current)],args.write,[('COMPTIME_BINDING75_SITE_COUNT',64+position),
        ('COMPTIME_BINDING75_FIRST_ARGUMENT',64+position+28),('COMPTIME_BINDING75_FIRST_PROVIDER',64+position+32)])
    print(json.dumps(dict(status='INDEPENDENT_COMPLETE_POPULATED_FIELDS_ONLY',bytes=len(current),sha256=hashlib.sha256(current).hexdigest(),model=model,product_writer_used=False),indent=2))

if __name__=='__main__':main()
