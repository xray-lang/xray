"""Independent contract76 vectors after complete field-model reproduction."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import struct
import sys
sys.dont_write_bytecode = True

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    source=args.source.resolve();folder=source/'tests/unit/xir'
    fixed=folder/'hash_protocol28_inputs';output=args.output.resolve();output.mkdir(parents=True,exist_ok=True);sys.path.insert(0,str(folder))
    previous=load(folder/'derive_hash75_vectors.py','members_previous75')
    nullable=load(folder/'derive_nullable75_vectors.py','members_nullable75')
    construction=load(folder/'derive_root_construction75_vectors.py','members_construction75')
    saved=sys.argv
    try:
        sys.argv=['independent-model-verification']
        with contextlib.redirect_stdout(io.StringIO()):
            previous.main();nullable.main()
            sys.argv=['construction-field-model','--source-root',str(source),'--output-root',str(source)]
            construction.main()
    finally:sys.argv=saved
    utility=load(folder/'derive_construction72_vectors.py','members_framing76')
    records=[]
    for path in [*sorted(fixed.glob('xir_*75_golden.h')),fixed/'compile_owner/checked_sizing75_golden.h']:
        rows=[]
        for name,data in previous.arrays(path):
            if '75' in name and data.startswith(b'XRCHK\0\0\0'):
                assert struct.unpack_from('<II',data,8)==(28,75)
                current=utility.frame(data[64:],28,76);rows.append((name.replace('75','76'),current))
                records.append(dict(symbol=rows[-1][0],bytes=len(current),sha256=hashlib.sha256(current).hexdigest()))
                if name=='checked_scalar75_golden':rows.append(('checked_scalar76_digest',current[32:64]))
            elif name.endswith('digest') and '75' in name:continue
            else:rows.append((name,data))
        macros=[(name.replace('75','76'),int(value)) for name,value in re.findall(r'#define (\w*75\w*) (\d+)u?',path.read_text(encoding='utf-8'))]
        target=output/path.name.replace('75','76');utility.header(target,target.stem.upper()+'_H',rows,True,macros)
        utility.header(path.with_name(target.name),target.stem.upper()+'_H',rows,args.write,macros)
    identities=load(folder/'derive_source_product_identity.py','members_product_identity76')
    rows=[item for mapping in identities.frames(25,76,28,30).values() for item in mapping.items()]
    rows += [(name.replace('_policy_value25','_policy75'),data)
             for mapping in identities.frames(25,75,28,30).values() for name,data in mapping.items()
             if '_policy_value25' in name]
    utility.header(output/'xir_hash76_identity.h','XIR_HASH76_IDENTITY_H',rows,True)
    utility.header(fixed/'xir_hash76_identity.h','XIR_HASH76_IDENTITY_H',rows,args.write)
    path=fixed/'root_construction75_goldens.h'
    rows=[(name.replace('75','76'),utility.frame(data[64:],28,76))
          for name,data in previous.arrays(path) if name in ('root_construction75_checked','root_construction75_instance')]
    macros=[(name.replace('75','76'),int(value)) for name,value in re.findall(r'#define (ROOT_CONSTRUCTION75_\w+) (\d+)u',path.read_text(encoding='utf-8'))]
    assert len(rows)==2 and len(macros)==2
    utility.header(output/'root_construction76_goldens.h','ROOT_CONSTRUCTION76_GOLDENS_H',rows,True,macros)
    utility.header(fixed/'root_construction76_goldens.h','ROOT_CONSTRUCTION76_GOLDENS_H',rows,args.write,macros)
    model=dict(linkage=0,functions=[dict(name='make',parameters=[],result=258,blocks=[[0,7,0,0]],instructions=[
        [154,256,0,0,0,0,0,0,0],[34,256,0,0,0,0,0,0,0],[2,2,0,0,0,0,7,0,0],
        [2,2,0,0,0,0,19,0,0],[159,0,0,3,0,0,0,0,0],[156,258,1,2,0,0,0,0,0],
        [33,0,5,0,0,0,0,0,0]],operands=[1,2,3])],declarations=None,generics=None,
        nodes=[[9,0,2,2],[10,0,2],[5,0,2]],nominals=[],interfaces=[],defaults=[])
    encoder=load(folder/'derive_callable71_checked_vectors.py','members_encoder76');body,_=encoder.encode_body(model,0)
    assert body[-8:]==bytes(8);body=body[:-8]+bytes(4)+body[-8:]
    data=utility.frame(body,28,76);utility.header(output/'xir_hash_members76_golden.h','XIR_HASH_MEMBERS76_GOLDEN_H',[('hash_members76_golden',data)],True)
    assert utility.literal(fixed/'xir_hash_members76_golden.h','hash_members76_golden')==data
    records.append(dict(symbol='hash_members76_golden',bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),opcodes=[154,34,2,2,159,156,33],operands=[1,2,3]))
    (output/'vectors.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps({'previous_complete_field_models':'VERIFIED','records':len(records),'new_member_packet':records[-1],'product_writer_used':False}))
if __name__=='__main__':main()
