"""Derive Channel descriptors from explicit fields, without a product writer."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def words(*values):
    return struct.pack('<'+'I'*len(values),*values)

def packet(element):
    name=b'channelTypes'
    # One Unit-returning function, two parameters, one block and literal RETURN=33.
    function=words(len(name))+name+words(2,256,257,0,1,0,1,0,0,1)
    instruction=words(33,0,0,0,0,0)+struct.pack('<q',0)+words(0,0)
    # No operand list, declarations, nominals, interfaces, construction, defaults or provenance.
    body=words(0,1,0)+function+instruction+words(0,0,2,0,0,9,0,element,9,0,3,0,0,0)
    prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,74,2,0,len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    args=parser.parse_args();root=Path(__file__).resolve().parents[3];test=root/'tests/unit/xir'
    helper=load(test/'derive_construction72_vectors.py','channel_fields')
    rows=[('channel_types74_golden',packet(2)),('channel_unit74_golden',packet(0))]
    helper.header(test/'xir_channel_types74_golden.h','XIR_CHANNEL_TYPES74_GOLDEN_H',rows,args.write)
    print(json.dumps({'writer_used':False,'schema':28,'semantic':74,'type_kind':9,
        'rows':[{'name':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for name,data in rows]},indent=2))
if __name__=='__main__':main()
