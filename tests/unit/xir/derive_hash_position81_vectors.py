"""Independent ordinal-read fields and complete retained80 models."""
import argparse,contextlib,copy,hashlib,io,json,re,sys
from pathlib import Path
sys.dont_write_bytecode=True

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    folder=args.source.resolve()/'tests/unit/xir'
    fixed=folder/'hash_protocol28_inputs';output=args.output.resolve();output.mkdir(parents=True,exist_ok=True);sys.path.insert(0,str(folder))
    import derive_map_membership80_vectors as previous
    import derive_hash76_vectors as loaders
    utility=loaders.load(folder/'derive_construction72_vectors.py','foreach_frames81');historical=loaders.load(folder/'derive_hash75_vectors.py','foreach_arrays81')
    saved=sys.argv
    try:
        sys.argv=['retained80','--source',str(args.source),'--output',str(output/'retained80')]
        with contextlib.redirect_stdout(io.StringIO()):previous.main()
    finally:sys.argv=saved
    records=[]
    for path in sorted((output/'retained80').glob('*80*golden*.h')):
        rows=[]
        for name,data in historical.arrays(path):
            if data.startswith(b'XRCHK\0\0\0'):
                data=utility.frame(data[64:],28,81);rows.append((name.replace('80','81'),data));records.append(dict(symbol=rows[-1][0],bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
                if name=='checked_scalar80_golden':rows.append(('checked_scalar81_digest',data[32:64]))
            elif name.endswith('digest') and '80' in name:continue
            else:rows.append((name,data))
        macros=[(name.replace('80','81'),int(value)) for name,value in re.findall(r'#define (\w*80\w*) (\d+)u?',path.read_text(encoding='utf-8'))]
        target=output/path.name.replace('80','81');utility.header(target,target.stem.upper()+'_H',rows,True,macros)
        destination=fixed/('compile_owner' if path.name.startswith('checked_sizing') else '')/target.name;utility.header(destination,target.stem.upper()+'_H',rows,args.write,macros)
    identities=loaders.load(folder/'derive_source_product_identity.py','foreach_identity81');rows=[item for mapping in identities.frames(25,81,28,30).values() for item in mapping.items()]
    for semantic in (75,76,77,78,79,80):rows += [(name.replace('_policy_value25','_policy'+str(semantic)),data) for mapping in identities.frames(25,semantic,28,30).values() for name,data in mapping.items() if '_policy_value25' in name]
    utility.header(output/'xir_hash81_identity.h','XIR_HASH81_IDENTITY_H',rows,True);utility.header(fixed/'xir_hash81_identity.h','XIR_HASH81_IDENTITY_H',rows,args.write)
    encoder=loaders.load(folder/'derive_callable71_checked_vectors.py','foreach_encoder81')
    def model(operation,is_map=True):
        result=1 if operation==175 else 2
        return dict(linkage=0,functions=[dict(name='position',parameters=[256,2],result=result,blocks=[[0,2,0,0]],instructions=[[operation,result,0,1,0,0,0,0,0],[33,0,2,0,0,0,0,0,0]],operands=[])],declarations=None,generics=None,nodes=[[9,0,2,1]] if is_map else [[10,0,2]],nominals=[],interfaces=[],defaults=[])
    models={'map_key':model(174),'map_value':model(175),'set_key':model(174,False)}
    models['bad_key_receiver']=copy.deepcopy(models['map_key']);models['bad_key_receiver']['nodes']=[[2,0,2]]
    models['bad_value_receiver']=copy.deepcopy(models['map_value']);models['bad_value_receiver']['nodes']=[[10,0,2]]
    for name,base,result in [('bad_key_result','map_key',1),('bad_value_result','map_value',2)]:
        models[name]=copy.deepcopy(models[base]);models[name]['functions'][0]['result']=result;models[name]['functions'][0]['instructions'][0][1]=result
    models['bad_index']=copy.deepcopy(models['map_key']);models['bad_index']['functions'][0]['parameters'][1]=5
    models['bad_key_proof']=copy.deepcopy(models['map_key']);models['bad_key_proof']['nodes']=[[2,0,2],[9,0,256,1]];models['bad_key_proof']['functions'][0]['parameters'][0]=257;models['bad_key_proof']['functions'][0]['result']=256;models['bad_key_proof']['functions'][0]['instructions'][0][1]=256
    rows=[]
    for name,description in models.items():
        body,_=encoder.encode_body(description,0);assert body[-8:]==bytes(8);data=utility.frame(body[:-8]+bytes(4)+body[-8:],28,81);rows.append(('hash_position81_'+name,data));records.append(dict(symbol=rows[-1][0],bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),model=description))
    utility.header(output/'xir_hash_position81_golden.h','XIR_HASH_POSITION81_GOLDEN_H',rows,True);utility.header(fixed/'xir_hash_position81_golden.h','XIR_HASH_POSITION81_GOLDEN_H',rows,args.write)
    (output/'vectors81.json').write_text(json.dumps(dict(previous80_complete_field_models='VERIFIED',records=records,product_writer_used=False),indent=2)+'\n',encoding='utf-8');print(json.dumps(dict(records=len(records),new_packets=9,product_writer_used=False)))

if __name__=='__main__':main()
