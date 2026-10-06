"""Independently frame current SDK identities from declared ABI facts, never a probe."""
from pathlib import Path
import argparse,hashlib,json,struct,sys,tempfile
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'scripts'))
from derive_xir_sdk_abi import prepare

def current_preimage(rows, semantic=68, value_abi=22, call_abi=28):
    word=lambda value:struct.pack('<I',value)
    def text(value):
        data=value.encode('utf-8');return word(len(data))+data
    prefix=[2,25,semantic,value_abi,call_abi,29,1,1,1,11,2,0,1,0,3,1,1]
    image=b'xray:xir-runtime-sdk:v1'+struct.pack('<17I',*prefix)
    image+=text('x86_64-windows-msvc')+text('xray:xir-runtime-abi-measurements:v1')
    image+=text('xray:xir-runtime-recipe:windows-x86_64-hosted:v1')+word(len(rows))
    image+=b''.join(struct.pack('<II',row['id'],row['value']) for row in rows)
    file_digest=hashlib.sha256(b'hi').digest()
    image+=word(1)+text('lib/test.lib')+word(5)+struct.pack('<Q',2)+file_digest
    image+=word(1)+text('kernel32')
    return image,file_digest,hashlib.sha256(image).digest()

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='xir-current-sdk-kat-') as temporary:
        facts=Path(temporary)/'facts';prepare(facts)
        rows=json.loads((facts/'EXPECTED.json').read_text())['rows']
    assert rows and len({row['id'] for row in rows})==len(rows)
    historical=json.loads(Path(__file__).with_name('sdk_historical_abi345.json').read_text(encoding='utf-8'))
    assert (historical['wire'],historical['value_abi'],historical['call_abi'],historical['program_abi'])==(25,21,26,29)
    assert historical['semantics']==[64,65] and len(historical['rows'])==345
    import re
    for semantic in historical['semantics']:
        previous_image,previous_file_digest,previous_digest=current_preimage(historical['rows'],semantic,21,26)
        previous_text=Path(__file__).with_name('sdk_previous'+str(semantic)+'_identity_golden.h').read_text(encoding='utf-8')
        for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
            match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete prior SDK KAT changed'
    previous_image,previous_file_digest,previous_digest=current_preimage(rows,66,22,28)
    previous_text=Path(__file__).with_name('sdk_previous66_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete 351-row66 SDK KAT changed'
    previous_image,previous_file_digest,previous_digest=current_preimage(rows,67,22,28)
    previous_text=Path(__file__).with_name('sdk_previous67_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete 351-row67 SDK KAT changed'
    image,file_digest,digest=current_preimage(rows)
    output='/* Independent current SDK framing and natural Windows C layout facts. */\n'
    for name,data in (('current_sdk_kat_preimage',image),('current_sdk_kat_file_digest',file_digest),('current_sdk_kat_digest',digest)):
        output+='static const uint8_t '+name+'[] = {\n'
        for at in range(0,len(data),16):output+='    '+','.join('0x'+format(x,'02x') for x in data[at:at+16])+',\n'
        output+='};\n'
    target=Path(__file__).with_name('sdk_current_identity_golden.h')
    if args.write:target.write_text(output,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==output,'current SDK known bytes differ from independent facts'
    print('Current SDK '+str(len(rows))+' fields / '+str(len(image))+' byte independent preimage PASS: '+digest.hex())

if __name__=='__main__':main()
