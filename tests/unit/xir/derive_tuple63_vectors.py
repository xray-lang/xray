"""Independent Tuple wire facts; no producer imports or descriptor aliases."""
from pathlib import Path
import argparse,hashlib,re,struct

def words(*values):
    return struct.pack('<'+'I'*len(values),*values)

def operation(code,ty=0,left=0,right=0,immediate=0):
    return words(code,ty,left,right,0,0)+struct.pack('<q',immediate)+words(0,0)

def tuple_packet():
    # One String parameter, Unit/i64/String fields, only two SSA payloads.
    ops=[operation(2,2,immediate=73),operation(135,256,right=2),operation(136,0,left=2),
         operation(136,3,left=2,immediate=2),operation(136,2,left=2,immediate=1),operation(25,left=5)]
    function=words(5)+b'tuple'+words(1,3,2,1,0,6,0,0,6)+b''.join(ops)+words(2,1,0)
    body=words(0,1,0)+function+words(0,1,0,0,6,0,3,0,2,3,0,0)
    prefix=b'XRCHK\0\0\0'+words(24,63,2,0)+struct.pack('<Q',len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body

def literal():
    data=tuple_packet()
    return ('/* Independent complete Tuple schema24/semantic63 packet. */\n'
            'static const unsigned char tuple_24_63[]={\n'+
            '\n'.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0,len(data),16))+'\n};\n')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    path=Path(__file__).with_name('tuple_24_63.inc.c')
    if args.write:path.write_text(literal(),encoding='utf-8')
    assert path.read_text(encoding='utf-8')==literal()
    data=tuple_packet();print('independent Tuple schema24/semantic63 KAT',len(data),hashlib.sha256(data).hexdigest())
if __name__=='__main__':main()
