"""Frame the unchanged complete predecessor bodies under the borrowed-type contract.

The predecessor headers are independent, checked-in field fixtures. No product
writer, generated C or runtime digest supplies an expectation. The new borrowed
descriptor packet is separately encoded from named fields below.
"""
from pathlib import Path
import argparse,hashlib,re,struct
from derive_callable71_checked_vectors import encode_body
from derive_source_product_identity import frames
from derive_construction72_vectors import header
HERE=Path(__file__).resolve().parent

def frame(body,semantic=84):
    prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',28,semantic,2,0,len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body

def arrays(text):
    return [(m,bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',m[2])))
        for m in re.finditer(r'static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};',text,re.S)]

def render(data):
    return '\n'+''.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+12])+',\n'
        for i in range(0,len(data),12))

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    args=parser.parse_args();packets=0;files=0
    for path in sorted(HERE.rglob('*83*golden*.h')):
        if path.name.startswith('sdk_'):continue
        text=path.read_text(encoding='utf-8');rows=arrays(text);replacements={}
        for match,data in rows:
            if data.startswith(b'XRCHK\0\0\0') and struct.unpack_from('<I',data,12)[0]==83:
                assert frame(data[64:],83)==data,(path.name,match[1])
                replacements[match[1]]=frame(data[64:]);packets+=1
        for match,data in reversed(rows):
            new=replacements.get(match[1],data)
            if match[1]=='checked_scalar83_digest':new=replacements['checked_scalar83_golden'][32:64]
            text=text[:match.start(2)]+render(new)+text[match.end(2):]
        text=re.sub(r'\b[A-Za-z_]\w*83\w*\b',lambda m:m[0].replace('83','84'),text)
        target=path.with_name(path.name.replace('83','84'))
        if args.write:target.write_text(text,encoding='utf-8',newline='\n')
        else:assert target.read_text(encoding='utf-8')==text,target
        files+=1
    print(f'Complete predecessor contract bodies: {packets} packets / {files} headers; product writer unused')
    model=dict(linkage=0,functions=[dict(name='viewTypes',parameters=[],result=0,
        blocks=[[0,1,0,0]],instructions=[[33,0,0,0,0,0,0,0,0]],operands=[])],
        declarations=None,generics=None,nominals=[],interfaces=[],defaults=[],nodes=[
            [11,0,2],[12,0,2],[5,0,256],[6,0,[258,257]],[2,0,2],[3,0,260],
            [1,0,[[257,0]],259,2]])
    body,_=encode_body(model,2)
    # The current wire stores the complete zero construction denominator before
    # defaults and provenance; the older field encoder predates this one word.
    body=body[:-8]+bytes(4)+body[-8:]
    packet=frame(body)
    text='/* Independent named fields for borrowed descriptor admission. */\n'
    text+='static const uint8_t view_types84_golden[] = {'+render(packet)+'};\n'
    target=HERE/'xir_view_types84_golden.h'
    if args.write:target.write_text(text,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==text,target
    print(f'New borrowed descriptor packet: {len(packet)} bytes / {hashlib.sha256(packet).hexdigest()}')
    identities=[row for mapping in frames(25,84).values() for row in mapping.items()]
    for semantic in range(75,84):
        identities.extend((name.replace('_value25',str(semantic)),data)
            for mapping in frames(25,semantic).values() for name,data in mapping.items() if 'policy' in name)
    header(HERE/'xir_hash84_identity.h','XIR_HASH84_IDENTITY_H',identities,args.write)

if __name__=='__main__':main()
