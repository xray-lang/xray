"""Derive current Library/default fields while reproducing whole retired vectors."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, re, struct

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--output-root',type=Path,required=True)
    parser.add_argument('--write',action='store_true');args=parser.parse_args()
    source=args.source_root/'tests/unit/xir';output=args.output_root/'tests/unit/xir'
    old=load(source/'derive_library2772_vectors.py','library_full_fields')
    utility=load(source/'derive_construction72_vectors.py','library_literal_fields')
    raw=(source/'library2671_models.json').read_bytes()
    # The frozen model digest uses Windows newlines; its JSON fields are fixed.
    framed=raw.replace(b'\r\n',b'\n').replace(b'\n',b'\r\n')
    assert hashlib.sha256(framed).hexdigest()==old.MODEL_SHA
    groups={};records=[];models=json.loads(raw)
    for name,model in models.items():
        family='library_string' if name.startswith('library_string') else 'defaults'
        filename='xir_library_string72_goldens.h' if family=='library_string' else 'xir_defaults72_golden_bytes.h'
        prior,_=old.frame(model,72,8,27,True)
        assert prior==utility.literal(source/filename,name.replace('71','72'))
        body,_=old.encode_body(model,8,True)
        position=len(body)-8-16*len(model['defaults'])
        assert struct.unpack('<I',body[position:position+4])[0]==len(model['defaults'])
        # The complete fixed models have zero nominals/materializations/sites/bindings.
        current=utility.frame(body[:position]+struct.pack('<III',0,0,0)+body[position:],30,75)
        if name=='defaults71_golden_8':assert current==utility.literal(source/'xir_checked_scalar75_golden.h','checked_scalar75_golden')
        if name=='defaults71_golden_9':assert current==utility.literal(source/'xir_generic_method75_golden.h','generic_method75_golden')
        groups.setdefault(family,[]).append((name.replace('71','75'),current))
        records.append(dict(name=name.replace('71','75'),bytes=len(current),sha256=hashlib.sha256(current).hexdigest(),default_count=64+position+12))
    for family,rows in groups.items():
        filename='xir_library_string75_goldens.h' if family=='library_string' else 'xir_defaults75_golden_bytes.h'
        macros=[]
        if family=='library_string':
            prior=(source/'xir_library_string72_goldens.h').read_text('utf-8')
            macros=[(name.replace('72','75'),int(value)) for name,value in re.findall(r'#define (LIBRARY_STRING72_\w+) (\d+)u',prior)]
        utility.header(output/filename,filename.upper().replace('.','_'),rows,args.write,macros)
    print(json.dumps(dict(status='INDEPENDENT_COMPLETE_FIELDS_ONLY',retired72_reproduced=True,product_writer_used=False,records=records),indent=2))

if __name__=='__main__':main()
