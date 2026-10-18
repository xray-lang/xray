"""Independently frame current SDK identities from declared ABI facts, never a probe."""
from pathlib import Path
import argparse,hashlib,json,struct,sys,tempfile
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'scripts'))
from derive_xir_sdk_abi import prepare

def current_preimage(rows, semantic=77, value_abi=29, call_abi=33, wire=30, program_abi=35):
    word=lambda value:struct.pack('<I',value)
    def text(value):
        data=value.encode('utf-8');return word(len(data))+data
    prefix_fields = (
        ('schema', 2), ('wire', wire), ('semantic', semantic),
        ('value_abi', value_abi), ('call_abi', call_abi), ('program_abi', program_abi),
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
    assert len(rows)==362 and len({row['id'] for row in rows})==362
    retired=json.loads(Path(__file__).with_name('sdk_historical_program34_abi363.json').read_text(encoding='utf-8'))
    assert (retired['wire'],retired['semantic'],retired['value_abi'],retired['call_abi'],retired['program_abi'])==(28,76,29,33,34)
    retired_rows=retired['rows'];assert len(retired_rows)==363
    assert any(row['id']==1505 for row in retired_rows) and all(row['id']!=1505 for row in rows)
    observer_ids={49,50,*range(4500,4509)}
    call32_rows=[dict(row,value=136) if row['id']==1000 else row for row in retired_rows if row['id']!=1013]
    assert len(call32_rows)==362
    old_rows=[row for row in call32_rows if row['id'] not in observer_ids]
    assert len(old_rows)==351
    historical=json.loads(Path(__file__).with_name('sdk_historical_abi345.json').read_text(encoding='utf-8'))
    assert (historical['wire'],historical['value_abi'],historical['call_abi'],historical['program_abi'])==(25,21,26,29)
    assert historical['semantics']==[64,65] and len(historical['rows'])==345
    import re
    for semantic in historical['semantics']:
        previous_image,previous_file_digest,previous_digest=current_preimage(historical['rows'],semantic,21,26,25,29)
        previous_text=Path(__file__).with_name('sdk_previous'+str(semantic)+'_identity_golden.h').read_text(encoding='utf-8')
        for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
            match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete prior SDK KAT changed'
    for semantic in (66,67,68,69,70,71):
        previous_image,previous_file_digest,previous_digest=current_preimage(old_rows,semantic,22,28,26 if semantic==71 else 25,29)
        previous_text=Path(__file__).with_name('sdk_previous'+str(semantic)+'_identity_golden.h').read_text(encoding='utf-8')
        for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
            match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete 351-row'+str(semantic)+' SDK KAT changed'
        print('Historical SDK '+str(26 if semantic==71 else 25)+'/'+str(semantic)+' complete '+str(len(previous_image))+' byte preimage/digest reproduced: '+previous_digest.hex())
    previous_image,previous_file_digest,previous_digest=current_preimage(old_rows,72,22,28,27,29)
    previous_text=Path(__file__).with_name('sdk_previous72_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',previous_image),('current_sdk_kat_file_digest',previous_file_digest),('current_sdk_kat_digest',previous_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete prior72 SDK KAT changed'
    value23_image,value23_file,value23_digest=current_preimage(old_rows,73,23,29,28,30)
    value23_text=Path(__file__).with_name('sdk_previous_value23_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',value23_image),('current_sdk_kat_file_digest',value23_file),('current_sdk_kat_digest',value23_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',value23_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete retired Value23 SDK KAT changed'
    value24_image,value24_file,value24_digest=current_preimage(call32_rows,73,24,29,28,30)
    value24_text=Path(__file__).with_name('sdk_previous_value24_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',value24_image),('current_sdk_kat_file_digest',value24_file),('current_sdk_kat_digest',value24_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',value24_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete retired Value24 SDK KAT changed'
    call32_image,call32_file,call32_digest=current_preimage(call32_rows,75,29,32,28,34)
    call32_text=Path(__file__).with_name('sdk_previous_call32_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',call32_image),('current_sdk_kat_file_digest',call32_file),('current_sdk_kat_digest',call32_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',call32_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete retired Call32 SDK KAT changed'
    assert len(call32_image)==3186
    semantic75_image,semantic75_file,semantic75_digest=current_preimage(retired_rows,75,29,33,28,34)
    semantic75_text=Path(__file__).with_name('sdk_previous_semantic75_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',semantic75_image),('current_sdk_kat_file_digest',semantic75_file),('current_sdk_kat_digest',semantic75_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',semantic75_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete prior semantic75 Call33 SDK KAT changed'
    retired_image,retired_file,retired_digest=current_preimage(retired_rows,76,29,33,28,34)
    retired_text=Path(__file__).with_name('sdk_previous_program34_identity_golden.h').read_text(encoding='utf-8')
    for name,data in (('current_sdk_kat_preimage',retired_image),('current_sdk_kat_file_digest',retired_file),('current_sdk_kat_digest',retired_digest)):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',retired_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete retired Program34 SDK KAT changed'
    assert len(retired_image)==3194
    ctfe=json.loads(Path(__file__).with_name('sdk_historical_ctfe75_abi.json').read_text(encoding='utf-8'))
    base_old=json.loads(Path(__file__).with_name('sdk_historical_program30_abi362.json').read_text(encoding='utf-8'))
    for name,model_rows,identity in [
        ('sdk_previous73_identity_golden.h',base_old['rows'],(73,24,29,28,30)),
        ('sdk_previous74_identity_golden.h',base_old['rows'],(74,24,29,29,30)),
        ('sdk_previous_ctfe75_identity_golden.h',ctfe['rows'],(75,24,29,30,31)),
        ('sdk_previous_channel76_identity_golden.h',rows,(76,29,33,28,35)),
    ]:
        prior_image,prior_file,prior_digest=current_preimage(model_rows,*identity)
        prior_text=Path(__file__).with_name(name).read_text(encoding='utf-8')
        for symbol,data in [('current_sdk_kat_preimage',prior_image),('current_sdk_kat_file_digest',prior_file),('current_sdk_kat_digest',prior_digest)]:
            match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,('complete predecessor SDK KAT changed',name)
    image,file_digest,digest=current_preimage(rows,77,29,33,30,35)
    identity_offset=len(b'xray:xir-runtime-sdk:v1')+4
    assert image[:identity_offset]==previous_image[:identity_offset]
    old_current_image,_,_=current_preimage(old_rows,75,25)
    assert old_current_image[identity_offset+20:]==previous_image[identity_offset+20:]
    assert struct.unpack('<5I',image[identity_offset:identity_offset+20])==(30,77,29,33,35)
    assert struct.unpack('<5I',previous_image[identity_offset:identity_offset+20])==(27,72,22,28,29)
    assert len(image)==3186 and len(previous_image)==len(value23_image)==3098
    assert file_digest==previous_file_digest and digest!=previous_digest
    output='/* Independent current SDK framing and natural Windows C layout facts. */\n'
    for name,data in (('current_sdk_kat_preimage',image),('current_sdk_kat_file_digest',file_digest),('current_sdk_kat_digest',digest)):
        output+='static const uint8_t '+name+'[] = {\n'
        for at in range(0,len(data),16):output+='    '+','.join('0x'+format(x,'02x') for x in data[at:at+16])+',\n'
        output+='};\n'
    target=Path(__file__).with_name('sdk_current_identity_golden.h')
    if args.write:target.write_text(output,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==output,'current SDK known bytes differ from independent facts'
    print('Current SDK '+str(len(current_rows))+' fields / '+str(len(image))+' byte independent preimage PASS: '+digest.hex())

if __name__=='__main__':main()
