"""Frame SourceProduct fixture identities independently of compiler output."""
from pathlib import Path
import argparse
import hashlib
import re
import struct

ROOT=Path(__file__).resolve().parent

def frames():
    # Result/parameter carriers are boxed 16/8. Frame storage is 8/8;
    # Unit has no frame payload and retains its UINT32_MAX slot sentinel.
    functions=[
        [0,1,2,8,16,8,1,0,0,0,0xffffffff,16,8,0],
        [1,0,1,0,16,8,0,0,0,0xffffffff],
        [2,0,2,8,16,8,0,0,0,0,0xffffffff],
    ]
    words=[1,1,24,30,30,2,3,1]+sum(functions,[])
    assert len(words)==43
    layout=b'xray:xir-lowered-layout:v1'+struct.pack('<43I',*words)
    result={'xir_source_product_identity.h':{
        'layout_current24_preimage':layout,
        'product_layout_value24_digest':hashlib.sha256(layout).digest(),
    }}
    policies={}
    for prefix in ('linked','original'):
        text=(prefix+'_source').encode('ascii')
        frame=b'xray:xir-c11-codegen-policy:v1'+struct.pack('<7I',1,28,84,24,30,30,len(text))+text
        policies[prefix+'_policy_value24_preimage']=frame
        policies[prefix+'_policy_value24']=hashlib.sha256(frame).digest()
    result['xir_native_projection_expectations.h']=policies
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    for filename,arrays in frames().items():
        path=ROOT/filename
        source=path.read_text(encoding='utf-8')
        for name,expected in arrays.items():
            pattern=r'(static const uint8_t '+name+r'\[\]\s*=\s*\{)(.*?)(\};)'
            match=re.search(pattern,source,re.S)
            assert match,name
            actual=bytes(int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            if args.write:
                rows='\n'+''.join('    '+','.join(f'0x{byte:02x}' for byte in expected[i:i+12])+',\n' for i in range(0,len(expected),12))
                source=source[:match.start(2)]+rows+source[match.end(2):]
            else:
                assert actual==expected,(filename,name,actual.hex(),expected.hex())
        if args.write:
            path.write_text(source,encoding='utf-8')
    print('Independent SourceProduct layout/policy preimages: PASS (Value24/Call30/Program30)')

if __name__=='__main__':
    main()
