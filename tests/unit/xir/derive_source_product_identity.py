"""Frame SourceProduct fixture identities independently of compiler output."""
from pathlib import Path
import argparse
import hashlib
import re
import struct

ROOT=Path(__file__).resolve().parent

def frames(value_abi=24, semantic=74, program_abi=30, wire=28):
    # Result/parameter carriers are boxed 16/8. Frame storage is 8/8;
    # Unit has no frame payload and retains its UINT32_MAX slot sentinel.
    functions=[
        [0,1,2,8,16,8,1,0,0,0,0xffffffff,16,8,0],
        [1,0,1,0,16,8,0,0,0,0xffffffff],
        [2,0,2,8,16,8,0,0,0,0,0xffffffff],
    ]
    words=[1,1,value_abi,29,program_abi,2,3,1]+sum(functions,[])
    assert len(words)==43
    layout=b'xray:xir-lowered-layout:v1'+struct.pack('<43I',*words)
    result={'xir_source_product_identity.h':{
        'layout_current'+str(value_abi)+'_preimage':layout,
        'product_layout_value'+str(value_abi)+'_digest':hashlib.sha256(layout).digest(),
    }}
    policies={}
    for prefix in ('linked','original'):
        text=(prefix+'_source').encode('ascii')
        frame=b'xray:xir-c11-codegen-policy:v1'+struct.pack('<7I',1,wire,semantic,value_abi,29,program_abi,len(text))+text
        policies[prefix+'_policy_value'+str(value_abi)+'_preimage']=frame
        policies[prefix+'_policy_value'+str(value_abi)]=hashlib.sha256(frame).digest()
    result['xir_native_projection_expectations.h']=policies
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    records=list(frames().items())
    baseline={name.replace('value24','value24_program31').replace('current24','current24_program31'):data
        for group in frames(24,73,31).values() for name,data in group.items()}
    retired={name.replace('value24','program30_value24').replace('current24','program30_value24'):data
        for group in frames(24,73,30).values() for name,data in group.items()}
    records.append(('xir_program31_value24_identity.h',{**baseline,**retired}))
    current={name.replace('27','27_program31'):data for group in frames(27,85,31).values() for name,data in group.items()}
    current_path=ROOT/'xir_view_value27_program31_identity.h'
    expected='/* Independent Value27/Call29/Program31 layout and code generation policy facts. */\n'
    expected+='#ifndef XIR_VIEW_VALUE27_PROGRAM31_IDENTITY_H\n#define XIR_VIEW_VALUE27_PROGRAM31_IDENTITY_H\n'
    for name,data in current.items():
        expected+='static const uint8_t '+name+'[]={\n'
        expected+=''.join('    '+','.join(f'0x{byte:02x}' for byte in data[i:i+12])+',\n' for i in range(0,len(data),12))
        expected+='};\n'
    expected+='#endif\n'
    if args.write:current_path.write_text(expected,encoding='utf-8',newline='\n')
    else:assert current_path.read_text(encoding='utf-8')==expected,'current Program31 frames differ'

    union={name+'_union86':data for group in frames(27,86,31,30).values() for name,data in group.items()}
    target=ROOT/'xir_union86_identity.h'
    text='/* Independent complete wire30/86, Value27/Call29/Program31 fields. */\n#ifndef XIR_UNION86_IDENTITY_H\n#define XIR_UNION86_IDENTITY_H\n'
    for name,data in union.items():
        text+='static const uint8_t '+name+'[]={\n'
        text+=''.join('    '+','.join(f'0x{byte:02x}' for byte in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};\n'
    text+='#endif\n'
    if args.write:target.write_text(text,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==text,'combined complete policy fields differ'
    for version in (25,26,27):
        arrays={name:data for group in frames(version,85 if version==27 else 84).values() for name,data in group.items()}
        records.append(('xir_hash84_identity.h' if version==25 else 'xir_view_value'+str(version)+'_identity.h',arrays))
    for filename,arrays in records:
        path=ROOT/filename
        source=path.read_text(encoding='utf-8')
        for name,expected in arrays.items():
            pattern=r'(static const uint8_t '+name+r'\[\]\s*=\s*\{)(.*?)(\};)'
            match=re.search(pattern,source,re.S)
            assert match,name
            actual=bytes(int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            assert actual==expected,(filename,name,actual.hex(),expected.hex())
    print('Independent SourceProduct complete Value24, retired84/Value25 and retired84/Value26 and current85/Value27 frames: PASS')

if __name__=='__main__':
    main()
