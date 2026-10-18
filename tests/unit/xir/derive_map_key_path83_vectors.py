"""Reconstruct complete current fields and preserve genuinely retired scalar frames."""
from pathlib import Path
import argparse,contextlib,hashlib,importlib.util,io,json,re,sys

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args();folder=args.source.resolve()/'tests/unit/xir'
    previous=load(folder/'derive_hash_position81_vectors.py','key_previous81_fields')
    reader=load(folder/'derive_hash75_vectors.py','key_named_field_arrays')
    utility=load(folder/'derive_construction72_vectors.py','key_complete_frame83')
    sizing=load(folder/'compile_owner/derive_checked_sizing74_golden.py','key_retired_scalar74')
    args.output.mkdir(parents=True,exist_ok=True)
    saved=sys.argv
    try:
        sys.argv=[str(previous.__file__),'--source',str(args.source),'--output',str(args.output/'retained81')]
        with contextlib.redirect_stdout(io.StringIO()):previous.main()
    finally:sys.argv=saved
    retired={}
    for suffix,name in [('golden',b'main'),('embedded_golden',b'm\0in'),('empty_golden',b'')]:
        packet=sizing.packet(name,28,74,33,True)
        assert packet==sizing.literal(folder/'compile_owner/checked_sizing74_golden.h','sizing74_'+suffix)
        retired['sizing74_retired_'+suffix]=packet
    records=[]
    for path in sorted((args.output/'retained81').glob('*81*golden*.h')):
        rows=[]
        for name,data in reader.arrays(path):
            if name in retired:
                data=retired[name];expected='XR_XIR_BAD_STRUCTURE'
            elif data.startswith(b'XRCHK\0\0\0'):
                data=utility.frame(data[64:],28,83)
                expected=('XR_XIR_BAD_STRUCTURE' if name in ('sizing81_embedded_golden','sizing81_empty_golden') else
                          'XR_XIR_BAD_TYPE' if '_bad_' in name or '_invalid_flags' in name else 'XR_XIR_OK')
            elif name.endswith('digest'):
                continue
            else:
                rows.append((name,data));continue
            symbol=name.replace('81','83');rows.append((symbol,data))
            records.append(dict(symbol=symbol,length=len(data),sha256=hashlib.sha256(data).hexdigest(),
                                wire=28,semantic=74 if name in retired else 83,expected=expected))
            if name=='checked_scalar81_golden':rows.append(('checked_scalar83_digest',data[32:64]))
        macros=[(name.replace('81','83'),int(value)) for name,value in re.findall(
            r'#define (\w*81\w*) (\d+)u?',path.read_text(encoding='utf-8'))]
        target=path.name.replace('81','83')
        utility.header(args.output/target,Path(target).stem.upper()+'_H',rows,True,macros)
        destination=folder/('compile_owner' if target.startswith('checked_sizing') else '')/target
        utility.header(destination,Path(target).stem.upper()+'_H',rows,args.write,macros)
    assert len(records)==60 and sum(row['semantic']==74 for row in records)==3
    (args.output/'vectors83.json').write_text(json.dumps(dict(records=records,
        product_writer_used=False,retired_frames_reconstructed=True),indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(records=len(records),current_frames=57,retired_frames=3,product_writer_used=False)))

if __name__=='__main__':main()
