"""Frame SourceProduct fixture identities independently of compiler output."""
from pathlib import Path
import argparse
import hashlib
import re
import struct

ROOT=Path(__file__).resolve().parent

def frames():
    # The carrier and function layouts are fixed independent facts. Only the
    # declared Program ABI word changes; all retired bytes remain checked.
    functions=[
        [0,1,2,8,16,8,1,0,0,0,0xffffffff,16,8,0],
        [1,0,1,0,16,8,0,0,0,0xffffffff],
        [2,0,2,8,16,8,0,0,0,0,0xffffffff],
    ]
    result={'xir_source_product_identity.h':{},'xir_native_projection_expectations.h':{}}
    for program in (30,33):
        words=[1,1,24,29,program,2,3,1]+sum(functions,[]);assert len(words)==43
        layout=b'xray:xir-lowered-layout:v1'+struct.pack('<43I',*words)
        key='layout_current24_preimage' if program==33 else 'layout_program30_preimage'
        digest='product_layout_value24_digest' if program==33 else 'product_layout_program30_digest'
        result['xir_source_product_identity.h'].update({key:layout,digest:hashlib.sha256(layout).digest()})
        for prefix in ('linked','original'):
            text=(prefix+'_source').encode('ascii')
            wire,semantic=(30,76) if program==33 else (28,73)
            frame=b'xray:xir-c11-codegen-policy:v1'+struct.pack('<7I',1,wire,semantic,24,29,program,len(text))+text
            suffix='value24' if program==33 else 'program30'
            result['xir_native_projection_expectations.h'][prefix+'_policy_'+suffix+'_preimage']=frame
            result['xir_native_projection_expectations.h'][prefix+'_policy_'+suffix]=hashlib.sha256(frame).digest()
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
            path.write_text(source,encoding='utf-8',newline='\n')
    print('Independent SourceProduct layout/policy preimages: PASS (Value24/Call29/Program33; complete Program30 vectors retained)')

if __name__=='__main__':
    main()
