"""Derive all Task role controls for the Channel semantic cutover."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def literal(helper,test,kind,name):
    for path in sorted(test.glob('xir_task_'+kind+'72_golden*.h')):
        if re.search(r'\b'+name+r'\s*\[',path.read_text()): return helper.literal(path,name)
    raise AssertionError(name)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[3]);parser.add_argument('--output-dir',type=Path);parser.add_argument('--write',action='store_true')
    args=parser.parse_args();test=args.root/'tests/unit/xir';output=args.output_dir or test;output.mkdir(parents=True,exist_ok=True)
    model=load(test/'derive_task_checked70_goldens.py','task74_fields')
    helper=load(test/'derive_construction72_vectors.py','task74_framing');rows=[]
    groups=[('types',[('golden',model.types_body(model.I64))]),('unit',[('golden',model.types_body(model.UNIT))]),
            ('go',[(role,model.go_body(role)) for role in model.ROLE_NAMES])]
    for kind,models in groups:
        encoded=[]
        for role,body70 in models:
            assert body70[-8:]==bytes(8)
            body=body70[:-8]+bytes(4)+body70[-8:]
            old=helper.frame(body,27,72)
            assert old==literal(helper,test,kind,'task_'+kind+'72_'+role)
            previous=helper.frame(body,28,73)
            if role=='golden' and kind in ['unit','go']:
                assert previous==helper.literal(test/('xir_task_'+kind+'73_golden.h'),'task_'+kind+'73_golden')
            current=helper.frame(body,28,74);assert current[64:]==old[64:]==previous[64:]
            encoded.append(('task_'+kind+'74_'+role,current))
            rows.append({'kind':kind,'role':role,'bytes':len(current),'previous72_exact':True,
                'previous73_exact':role=='golden' and kind in ['unit','go'],
                'body_sha256':hashlib.sha256(body).hexdigest(),'current_sha256':hashlib.sha256(current).hexdigest()})
        helper.header(output/('xir_task_'+kind+'74_golden.h'),'XIR_TASK_'+kind.upper()+'74_GOLDEN_H',encoded,args.write or args.output_dir is not None)
    print(json.dumps({'writer_used':False,'schema':28,'semantic':74,'rows':rows},indent=2))
if __name__=='__main__':main()
