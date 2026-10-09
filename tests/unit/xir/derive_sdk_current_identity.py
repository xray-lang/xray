"""Independently frame current SDK identities from declared ABI facts, never a probe."""
from pathlib import Path
import argparse,hashlib,json,struct,sys,tempfile
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'scripts'))
from derive_xir_sdk_abi import prepare

def current_preimage(rows, semantic=72, value_abi=22, call_abi=28, wire=27):
    word=lambda value:struct.pack('<I',value)
    def text(value):
        data=value.encode('utf-8');return word(len(data))+data
    prefix_fields = (
        ('schema', 2), ('wire', wire), ('semantic', semantic),
        ('value_abi', value_abi), ('call_abi', call_abi), ('program_abi', 29),
        ('architecture', 1), ('object_format', 1), ('hosted', 1),
        ('c_dialect', 11), ('crt', 2), ('sanitizers', 0), ('allocator', 1),
        ('assertions', 0), ('build_provider', 3), ('abi_recipe_version', 1),
        ('closure_recipe_version', 1),
    )
    prefix=[value for name,value in prefix_fields]
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
    assert len(rows)==351 and len({row['id'] for row in rows})==351
    historical=json.loads(Path(__file__).with_name('sdk_historical_abi345.json').read_text(encoding='utf-8'))
    assert (historical['wire'],historical['value_abi'],historical['call_abi'],historical['program_abi'])==(25,21,26,29)
    assert historical['semantics']==[64,65] and len(historical['rows'])==345
    import re
    for semantic in historical['semantics']:
        previous_image,previous_file_digest,previous_digest=current_preimage(historical['rows'],semantic,21,26,25)
        previous_text=Path(__file__).with_name('sdk_previous'+str(semantic)+'_identity_golden.h').read_text(encoding='utf-8')
        for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
            match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete prior SDK KAT changed'
    for semantic in (66,67,68,69,70,71):
        previous_image,previous_file_digest,previous_digest=current_preimage(rows,semantic,22,28,26 if semantic==71 else 25)
        previous_text=Path(__file__).with_name('sdk_previous'+str(semantic)+'_identity_golden.h').read_text(encoding='utf-8')
        for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
            match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete 351-row'+str(semantic)+' SDK KAT changed'
        print('Historical SDK '+str(26 if semantic==71 else 25)+'/'+str(semantic)+' complete '+str(len(previous_image))+' byte preimage/digest reproduced: '+previous_digest.hex())
    image,file_digest,digest=current_preimage(rows)
    wire_offset=len(b'xray:xir-runtime-sdk:v1')+4
    semantic_offset=wire_offset+4
    assert image[:wire_offset]==previous_image[:wire_offset]
    assert image[semantic_offset+4:]==previous_image[semantic_offset+4:]
    assert struct.unpack('<II',image[wire_offset:semantic_offset+4])==(27,72)
    assert struct.unpack('<II',previous_image[wire_offset:semantic_offset+4])==(26,71)
    assert len(image)==len(previous_image)==3098
    assert file_digest==previous_file_digest and digest!=previous_digest
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
