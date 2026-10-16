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
    layouts={}
    for value,call,program in ((24,29,30),(29,33,34)):
        words=[1,1,value,call,program,2,3,1]+sum(functions,[])
        assert len(words)==43
        layout=b'xray:xir-lowered-layout:v1'+struct.pack('<43I',*words)
        layouts[f'layout_current{value}_preimage']=layout
        layouts[f'product_layout_value{value}_digest']=hashlib.sha256(layout).digest()
    result={'xir_source_product_identity.h':layouts}
    policies={}
    for prefix in ('linked','original'):
        text=(prefix+'_source').encode('ascii')
        for semantic,value,call,program in ((73,24,29,30),(76,29,33,34)):
            frame=b'xray:xir-c11-codegen-policy:v1'+struct.pack('<7I',1,28,semantic,value,call,program,len(text))+text
            policies[prefix+f'_policy_value{value}_preimage']=frame
            policies[prefix+f'_policy_value{value}']=hashlib.sha256(frame).digest()
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
    print('Independent current Value29/Call33/Program34 and retained Value24/Call29/Program30 layout/policy preimages: PASS')

if __name__=='__main__':
    main()
