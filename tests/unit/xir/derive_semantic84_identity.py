"""Frame complete lowered layout and C policy facts without executing a writer."""
from pathlib import Path
import argparse,hashlib,struct
from derive_construction72_vectors import header
ROOT=Path(__file__).resolve().parent
def frames():
    functions=[[0,1,2,8,16,8,1,0,0,0,0xffffffff,16,8,0],
               [1,0,1,0,16,8,0,0,0,0xffffffff],
               [2,0,2,8,16,8,0,0,0,0,0xffffffff]]
    words=[1,1,25,29,34,2,3,1]+sum(functions,[]);assert len(words)==43
    layout=b'xray:xir-lowered-layout:v1'+struct.pack('<43I',*words)
    rows=[('layout_current84_preimage',layout),('product_layout84_digest',hashlib.sha256(layout).digest())]
    for prefix in ('linked','original'):
        text=(prefix+'_source').encode('ascii')
        policy=b'xray:xir-c11-codegen-policy:v1'+struct.pack('<7I',1,32,84,25,29,34,len(text))+text
        rows.extend([(prefix+'_policy84_preimage',policy),(prefix+'_policy84',hashlib.sha256(policy).digest())])
    return rows
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    header(ROOT/'xir_semantic84_identity.h','XIR_SEMANTIC84_IDENTITY_H',frames(),args.write)
    print('Independent unified layout/policy identity: 32/84/25/29/34 PASS')
if __name__=='__main__':main()
