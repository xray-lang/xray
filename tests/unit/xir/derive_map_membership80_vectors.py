"""Independent ordinary Map membership fields and complete retained79 models."""
import argparse,contextlib,copy,hashlib,io,json,re,sys
from pathlib import Path
sys.dont_write_bytecode=True

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    folder=args.source.resolve()/'tests/unit/xir'
    fixed=folder/'hash_protocol28_inputs';output=args.output.resolve();output.mkdir(parents=True,exist_ok=True);sys.path.insert(0,str(folder))
    import derive_set_algebra79_vectors as previous
    import derive_hash76_vectors as loaders
    utility=loaders.load(folder/'derive_construction72_vectors.py','map_frames80');historical=loaders.load(folder/'derive_hash75_vectors.py','map_arrays80')
    saved=sys.argv
    try:
        sys.argv=['retained79','--source',str(args.source),'--output',str(output/'retained79')]
        with contextlib.redirect_stdout(io.StringIO()):previous.main()
    finally:sys.argv=saved
    records=[]
    for path in sorted((output/'retained79').glob('*79*golden*.h')):
        rows=[]
        for name,data in historical.arrays(path):
            if data.startswith(b'XRCHK\0\0\0'):
                data=utility.frame(data[64:],28,80);rows.append((name.replace('79','80'),data));records.append(dict(symbol=rows[-1][0],bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
                if name=='checked_scalar79_golden':rows.append(('checked_scalar80_digest',data[32:64]))
            elif name.endswith('digest') and '79' in name:continue
            else:rows.append((name,data))
        macros=[(name.replace('79','80'),int(value)) for name,value in re.findall(r'#define (\w*79\w*) (\d+)u?',path.read_text(encoding='utf-8'))]
        target=output/path.name.replace('79','80');utility.header(target,target.stem.upper()+'_H',rows,True,macros)
        destination=fixed/('compile_owner' if path.name.startswith('checked_sizing') else '')/target.name;utility.header(destination,target.stem.upper()+'_H',rows,args.write,macros)
    identities=loaders.load(folder/'derive_source_product_identity.py','map_identity80');rows=[item for mapping in identities.frames(25,80,28,30).values() for item in mapping.items()]
    for semantic in (75,76,77,78,79):rows += [(name.replace('_policy_value25','_policy'+str(semantic)),data) for mapping in identities.frames(25,semantic,28,30).values() for name,data in mapping.items() if '_policy_value25' in name]
    utility.header(output/'xir_hash80_identity.h','XIR_HASH80_IDENTITY_H',rows,True);utility.header(fixed/'xir_hash80_identity.h','XIR_HASH80_IDENTITY_H',rows,args.write)
    encoder=loaders.load(folder/'derive_callable71_checked_vectors.py','map_encoder80')
    valid=dict(linkage=0,functions=[dict(name='membership',parameters=[256,2],result=1,blocks=[[0,2,0,0]],instructions=[[173,1,0,1,0,0,0,0,0],[33,0,2,0,0,0,0,0,0]],operands=[])],declarations=None,generics=None,nodes=[[9,0,2,2]],nominals=[],interfaces=[],defaults=[])
    models={'valid':valid}
    models['bad_receiver']=copy.deepcopy(valid);models['bad_receiver']['nodes']=[[10,0,2]]
    models['bad_value_width']=copy.deepcopy(valid);models['bad_value_width']['functions'][0]['parameters'][1]=5
    models['bad_result']=copy.deepcopy(valid);models['bad_result']['functions'][0]['result']=2;models['bad_result']['functions'][0]['instructions'][0][1]=2
    models['bad_equal']=copy.deepcopy(valid);models['bad_equal']['nodes']=[[10,0,2],[9,0,2,256]];models['bad_equal']['functions'][0]['parameters']=[257,256]
    models['bad_key_proof']=copy.deepcopy(valid);models['bad_key_proof']['nodes']=[[9,0,257,2],[2,0,2]]
    rows=[]
    for name,description in models.items():
        body,_=encoder.encode_body(description,0);assert body[-8:]==bytes(8);data=utility.frame(body[:-8]+bytes(4)+body[-8:],28,80);rows.append(('map_membership80_'+name,data));records.append(dict(symbol=rows[-1][0],bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),model=description))
    utility.header(output/'xir_map_membership80_golden.h','XIR_MAP_MEMBERSHIP80_GOLDEN_H',rows,True);utility.header(fixed/'xir_map_membership80_golden.h','XIR_MAP_MEMBERSHIP80_GOLDEN_H',rows,args.write)
    (output/'vectors80.json').write_text(json.dumps(dict(previous79_complete_field_models='VERIFIED',records=records,product_writer_used=False),indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(records=len(records),new_packets=6,product_writer_used=False)))

if __name__=='__main__':main()
