"""Independently frame current SDK identities from declared ABI facts, never a probe."""
from pathlib import Path
import argparse,hashlib,json,struct,sys,tempfile
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'scripts'))
from derive_xir_sdk_abi import prepare

def current_preimage(rows, semantic=77, value_abi=29, call_abi=33, wire=30, program_abi=35,
                     system_libraries=('kernel32',)):
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
    image+=word(len(system_libraries))+b''.join(text(name) for name in system_libraries)
    return image,file_digest,hashlib.sha256(image).digest()

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='xir-current-sdk-kat-') as temporary:
        facts=Path(temporary)/'facts';prepare(facts)
        rows=json.loads((facts/'EXPECTED.json').read_text())['rows']
    active_rows=rows
    assert len(active_rows)==386 and len({row['id'] for row in active_rows})==386
    parent_rows={row['id']:row for row in active_rows if row['id'] in (3014,3113)}
    assert {key:row['value'] for key,row in parent_rows.items()}=={3014:124,3113:108}
    prior_layout_rows=[row for row in active_rows if row['id'] not in parent_rows]
    assert len(prior_layout_rows)==384
    # Historical derivations use their immutable complete frame, not today's layout.
    import re
    historical100=Path(__file__).with_name('sdk_previous39_100_identity_golden.h').read_text()
    fixed=re.search(r'static const uint8_t previous39_100_sdk_kat_preimage\[\] = \{(.*?)\};',historical100,re.S)
    fixed=bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',fixed[1]))
    offset=len(b'xray:xir-runtime-sdk:v1')+17*4
    for _ in range(3):
        length=struct.unpack_from('<I',fixed,offset)[0];offset+=4+length
    count=struct.unpack_from('<I',fixed,offset)[0];offset+=4
    by_id={row['id']:row for row in active_rows}
    current_rows=[]
    for i in range(count):
        identifier,value=struct.unpack_from('<II',fixed,offset+8*i)
        current_rows.append(dict(by_id[identifier],value=value))
    assert len(current_rows)==380
    old100,old100_file,old100_digest=current_preimage(current_rows,100,30,35,39,37)
    assert old100==fixed
    for suffix,data in [('file_digest',old100_file),('digest',old100_digest)]:
        saved=re.search(r'static const uint8_t previous39_100_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',historical100,re.S)
        assert bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',saved[1]))==data
    # Retired wire images retain their independently declared resource layout.
    # These fixed historical facts never enter the current ABI derivation.
    historical_resources={}
    for base,name,size,fields in [
        (2500,'XrCompileResourceLimits',24,['allocated_bytes','live_bytes','work']),
        (2600,'XrCompileResourceStats',40,['allocation_count','allocated_bytes','live_bytes','peak_bytes','work']),
    ]:
        historical_resources[base]={'id':base,'name':name+'.size','value':size,'expression':'sizeof('+name+')'}
        historical_resources[base+1]={'id':base+1,'name':name+'.align','value':8,'expression':'_Alignof('+name+')'}
        for index,field in enumerate(fields):
            identifier=base+2+index
            historical_resources[identifier]={'id':identifier,'name':name+'.'+field+'.offset','value':index*8,
                'expression':'offsetof('+name+','+field+')'}
    frozen_rows=[]
    for row in current_rows:
        frozen_rows.append(historical_resources.get(row['id'],row))
        if row['id']==2605:frozen_rows.append(historical_resources[2606])
    assert len(frozen_rows)==381 and len({row['id'] for row in frozen_rows})==381
    previous_current_rows=[row for row in frozen_rows if row['id']!=3710]
    parents=json.loads(Path(__file__).with_name('sdk_historical_shared_parents.json').read_text())
    rows=[row for row in previous_current_rows if row['id']<4600]
    assert len(rows)==362
    assert rows==next(parent['rows'] for parent in parents['parents'] if parent['tag']=='p18')
    import re
    for parent in parents['parents']:
        wire,semantic,value,call,program=parent['identity']
        image,file_digest,digest=current_preimage(parent['rows'],semantic,value,call,wire,program)
        header=Path(__file__).with_name(parent['header']).read_text()
        for name,data in [('current_sdk_kat_preimage',image),('current_sdk_kat_file_digest',file_digest),('current_sdk_kat_digest',digest)]:
            match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',header,re.S)
            assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,('complete fixed-parent SDK changed',parent['tag'])

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
        ('sdk_previous_p18legacy74_identity_golden.h',base_old['rows'],(74,24,29,29,30)),
        ('sdk_previous_ctfe75_identity_golden.h',ctfe['rows'],(75,24,29,30,31)),
        ('sdk_previous_channel76_identity_golden.h',rows,(76,29,33,28,35)),
    ]:
        prior_image,prior_file,prior_digest=current_preimage(model_rows,*identity)
        prior_text=Path(__file__).with_name(name).read_text(encoding='utf-8')
        for symbol,data in [('current_sdk_kat_preimage',prior_image),('current_sdk_kat_file_digest',prior_file),('current_sdk_kat_digest',prior_digest)]:
            match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_text,re.S)
            assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,('complete predecessor SDK KAT changed',name)
    prior_image,prior_file,prior_digest=current_preimage(previous_current_rows,90,30,35,32,36)
    prior_text=Path(__file__).with_name('sdk_previous90_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('current_sdk_kat_preimage',prior_image),('current_sdk_kat_file_digest',prior_file),('current_sdk_kat_digest',prior_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete semantic90 SDK KAT changed'
    historical_image,historical_file,historical_digest=current_preimage(previous_current_rows,91,30,35,32,36)
    historical_text=Path(__file__).with_name('sdk_previous32_91_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('current_sdk_kat_preimage',historical_image),('current_sdk_kat_file_digest',historical_file),('current_sdk_kat_digest',historical_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',historical_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete historical32/91 bytes changed'
    prior_slot_image,prior_slot_file,prior_slot_digest=current_preimage(frozen_rows,96,30,35,36,36)
    prior_slot_text=Path(__file__).with_name('sdk_previous36_96_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('current_sdk_kat_preimage',prior_slot_image),('current_sdk_kat_file_digest',prior_slot_file),('current_sdk_kat_digest',prior_slot_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_slot_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete predecessor36/96 bytes changed'
    prior_bytes_image,prior_bytes_file,prior_bytes_digest=current_preimage(frozen_rows,97,30,35,37,36)
    assert prior_bytes_digest.hex()=='8bc0ef960bfb32df930e20c8fce81adad1248b4227ff7ba34056f68b60226b84'
    prior_bytes_text=Path(__file__).with_name('sdk_current_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('previous37_97_sdk_kat_preimage',prior_bytes_image),('previous37_97_sdk_kat_file_digest',prior_bytes_file),('previous37_97_sdk_kat_digest',prior_bytes_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_bytes_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete predecessor37/97 bytes changed'
    prior_new_resources_image,prior_new_resources_file,prior_new_resources_digest=current_preimage(current_rows,98,30,35,37,36)
    prior_new_resources_text=Path(__file__).with_name('sdk_current_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('previous37_new_resources_sdk_kat_preimage',prior_new_resources_image),('previous37_new_resources_sdk_kat_file_digest',prior_new_resources_file),('previous37_new_resources_sdk_kat_digest',prior_new_resources_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_new_resources_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete prior37 new-resource bytes changed'
    prior_slice_image,prior_slice_file,prior_slice_digest=current_preimage(current_rows,98,30,35,38,36)
    prior_slice_text=Path(__file__).with_name('sdk_current_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('previous38_98_sdk_kat_preimage',prior_slice_image),('previous38_98_sdk_kat_file_digest',prior_slice_file),('previous38_98_sdk_kat_digest',prior_slice_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_slice_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete predecessor38/98 bytes changed'
    prior_native_image,prior_native_file,prior_native_digest=current_preimage(current_rows,99,30,35,38,36)
    prior_native_text=Path(__file__).with_name('sdk_current_identity_golden.h').read_text(encoding='utf-8')
    for symbol,data in [('previous38_99_sdk_kat_preimage',prior_native_image),('previous38_99_sdk_kat_file_digest',prior_native_file),('previous38_99_sdk_kat_digest',prior_native_digest)]:
        match=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',prior_native_text,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete predecessor38/99 bytes changed'
    # Preserve the complete current108 frame before deriving its successor.
    prior108=Path(__file__).with_name('sdk_previous39_108_identity_golden.h').read_text(encoding='utf-8')
    previous108=current_preimage(prior_layout_rows,108,30,37,39,37)
    for suffix,data in zip(('preimage','file_digest','digest'),previous108):
        match=re.search(r'static const uint8_t previous39_108_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior108,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common108 SDK KAT changed'
    prior110=Path(__file__).with_name('sdk_previous39_110_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,110,30,37,39,37)):
        match=re.search(r'static const uint8_t previous39_110_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior110,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common110 SDK KAT changed'
    prior111=Path(__file__).with_name('sdk_previous39_111_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,111,30,37,39,37)):
        match=re.search(r'static const uint8_t previous39_111_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior111,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common111 SDK KAT changed'
    prior112=Path(__file__).with_name('sdk_previous39_112_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,112,30,37,39,37)):
        match=re.search(r'static const uint8_t previous39_112_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior112,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common112 SDK KAT changed'
    prior113=Path(__file__).with_name('sdk_previous39_113_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,113,30,37,39,37)):
        match=re.search(r'static const uint8_t previous39_113_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior113,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common113 SDK KAT changed'
    prior114=Path(__file__).with_name('sdk_previous39_114_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,114,30,37,39,37)):
        match=re.search(r'static const uint8_t previous39_114_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior114,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common114 SDK KAT changed'
    prior115=Path(__file__).with_name('sdk_previous39_115_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,115,30,37,39,37)):
        match=re.search(r'static const uint8_t previous39_115_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior115,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common115 SDK KAT changed'
    prior116=Path(__file__).with_name('sdk_previous39_116_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,116,30,37,39,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous39_116_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior116,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common116 SDK KAT changed'
    prior117=Path(__file__).with_name('sdk_previous39_117_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,117,30,37,39,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous39_117_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior117,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common117 SDK KAT changed'
    prior118=Path(__file__).with_name('sdk_previous40_118_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,118,31,38,40,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous40_118_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior118,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common118 SDK KAT changed'
    prior119=Path(__file__).with_name('sdk_previous40_119_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,119,31,38,40,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous40_119_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior119,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common119 SDK KAT changed'
    prior120=Path(__file__).with_name('sdk_previous41_120_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,120,31,38,41,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous41_120_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior120,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common120 SDK KAT changed'
    prior121=Path(__file__).with_name('sdk_previous41_121_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,121,31,38,41,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous41_121_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior121,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common121 SDK KAT changed'
    prior122=Path(__file__).with_name('sdk_previous41_122_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,122,31,38,41,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous41_122_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior122,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common122 SDK KAT changed'
    prior123=Path(__file__).with_name('sdk_previous41_123_identity_golden.h').read_text(encoding='utf-8')
    for suffix,data in zip(('preimage','file_digest','digest'),current_preimage(prior_layout_rows,123,31,38,41,37,('kernel32','bcrypt'))):
        match=re.search(r'static const uint8_t previous41_123_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior123,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common123 SDK KAT changed'
    prior124=Path(__file__).with_name('sdk_previous41_124_identity_golden.h').read_text(encoding='utf-8')
    previous124=current_preimage(prior_layout_rows,124,31,38,41,37,('kernel32','bcrypt'))
    assert len(previous124[0])==3372
    assert previous124[2].hex()=='7deba9c64a76318f2bf45e9ecc055e85d7fe81e551a247fa96ab95b72f08405c'
    for suffix,data in zip(('preimage','file_digest','digest'),previous124):
        match=re.search(r'static const uint8_t previous41_124_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior124,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common124 SDK KAT changed'
    prior125=Path(__file__).with_name('sdk_previous42_125_identity_golden.h').read_text(encoding='utf-8')
    previous125=current_preimage(active_rows,125,31,38,42,37,('kernel32','bcrypt'))
    assert len(previous125[0])==3388
    assert previous125[2].hex()=='75ab4205050a1c52ba8fde2632128561228b4f7200cb4200011196dc09dab3ea'
    for suffix,data in zip(('preimage','file_digest','digest'),previous125):
        match=re.search(r'static const uint8_t previous42_125_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',prior125,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete common125 SDK KAT changed'
    missing_advapi32=current_preimage(active_rows,126,31,38,42,37,('kernel32','bcrypt'))
    missing_text=Path(__file__).with_name('sdk_missing_advapi32_identity_golden.h').read_text(encoding='utf-8')
    assert len(missing_advapi32[0])==3388
    assert missing_advapi32[2].hex()=='76adadcee757328faa7633f5c22b9afe0049366437671fd58960338e60d7c493'
    for suffix,data in zip(('preimage','file_digest','digest'),missing_advapi32):
        match=re.search(r'static const uint8_t missing_advapi32_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',missing_text,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete missing-Advapi32 SDK KAT changed'
    missing_userenv=current_preimage(active_rows,126,31,38,42,37,('kernel32','bcrypt','advapi32'))
    missing_text=Path(__file__).with_name('sdk_missing_userenv_identity_golden.h').read_text(encoding='utf-8')
    assert len(missing_userenv[0])==3400
    assert missing_userenv[2].hex()=='cd037bb91628d270866d38e2bd66958edbf22b5b1921cf46d47894aec51cb072'
    for suffix,data in zip(('preimage','file_digest','digest'),missing_userenv):
        match=re.search(r'static const uint8_t missing_userenv_sdk_kat_'+suffix+r'\[\] = \{(.*?)\};',missing_text,re.S)
        assert match and bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',match[1]))==data,'complete missing-Userenv SDK KAT changed'
    previous126=current_preimage(active_rows,126,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    assert len(previous126[0])==3411
    assert previous126[2].hex()=='d65d769babfe02bbf17d19a5a73d92abe0726464767d91f7a917d690a7f650cb'
    previous127=current_preimage(active_rows,127,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    assert len(previous127[0])==3411
    assert previous127[2].hex()=='9e01ef2f2dda74ca24302d84c15d737eb4f9a8b82502e994bdaed06d297ae07f'
    previous127_missing_advapi32=current_preimage(active_rows,127,31,38,42,37,('kernel32','bcrypt'))
    previous127_missing_userenv=current_preimage(active_rows,127,31,38,42,37,('kernel32','bcrypt','advapi32'))
    previous128=current_preimage(active_rows,128,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    assert previous128[2].hex()=='812de1c1764b931c77a4a38c2a8601a7e8df051acf89be93d4a0c658e60d7acd'
    previous128_missing_advapi32=current_preimage(active_rows,128,31,38,42,37,('kernel32','bcrypt'))
    previous128_missing_userenv=current_preimage(active_rows,128,31,38,42,37,('kernel32','bcrypt','advapi32'))
    previous129=current_preimage(active_rows,129,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    previous130=current_preimage(active_rows,130,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    previous131=current_preimage(active_rows,131,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    previous132=current_preimage(active_rows,132,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    previous133=current_preimage(active_rows,133,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    previous134=current_preimage(active_rows,134,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    current_missing_advapi32=current_preimage(active_rows,135,31,38,42,37,('kernel32','bcrypt'))
    current_missing_userenv=current_preimage(active_rows,135,31,38,42,37,('kernel32','bcrypt','advapi32'))
    image,file_digest,digest=current_preimage(active_rows,135,31,38,42,37,('kernel32','bcrypt','advapi32','userenv'))
    assert image[:-49]==current_missing_advapi32[0][:-26] and len(image)==len(current_missing_advapi32[0])+23
    assert image[:-49]==current_missing_userenv[0][:-38] and len(image)==len(current_missing_userenv[0])+11
    assert digest!=current_missing_advapi32[2] and digest!=current_missing_userenv[2] and digest!=previous126[2] and digest!=previous127[2] and digest!=previous128[2]
    prior_resources_image,prior_resources_file,prior_resources_digest=current_preimage(frozen_rows,98,30,35,37,36)
    assert len(prior_resources_image)==3338
    assert prior_resources_digest.hex()=='8fd650c91daccdcad55f6bf8c40b5f4e2f9ed9611d721eff0c26321e84adeca1'
    identity_offset=len(b'xray:xir-runtime-sdk:v1')+4
    assert image[:identity_offset]==previous_image[:identity_offset]
    old_current_image,_,_=current_preimage(old_rows,75,25)
    assert old_current_image[identity_offset+20:]==previous_image[identity_offset+20:]
    assert struct.unpack('<5I',image[identity_offset:identity_offset+20])==(42,135,31,38,37)
    assert struct.unpack('<5I',previous_image[identity_offset:identity_offset+20])==(27,72,22,28,29)
    assert len(image)==3411 and len(historical_image)==3330 and len(previous_image)==len(value23_image)==3098
    assert file_digest==previous_file_digest and digest!=previous_digest
    output='/* Independent current SDK framing and natural Windows C layout facts. */\n'
    for name,data in (('current_sdk_kat_preimage',image),('current_sdk_kat_file_digest',file_digest),('current_sdk_kat_digest',digest),
            ('previous42_126_sdk_kat_preimage',previous126[0]),('previous42_126_sdk_kat_file_digest',previous126[1]),('previous42_126_sdk_kat_digest',previous126[2]),
            ('previous42_127_sdk_kat_preimage',previous127[0]),('previous42_127_sdk_kat_file_digest',previous127[1]),('previous42_127_sdk_kat_digest',previous127[2]),
            ('previous42_127_missing_advapi32_sdk_kat_preimage',previous127_missing_advapi32[0]),('previous42_127_missing_advapi32_sdk_kat_file_digest',previous127_missing_advapi32[1]),('previous42_127_missing_advapi32_sdk_kat_digest',previous127_missing_advapi32[2]),
            ('previous42_127_missing_userenv_sdk_kat_preimage',previous127_missing_userenv[0]),('previous42_127_missing_userenv_sdk_kat_file_digest',previous127_missing_userenv[1]),('previous42_127_missing_userenv_sdk_kat_digest',previous127_missing_userenv[2]),
            ('previous42_128_sdk_kat_preimage',previous128[0]),('previous42_128_sdk_kat_file_digest',previous128[1]),('previous42_128_sdk_kat_digest',previous128[2]),
            ('previous42_128_missing_advapi32_sdk_kat_preimage',previous128_missing_advapi32[0]),('previous42_128_missing_advapi32_sdk_kat_file_digest',previous128_missing_advapi32[1]),('previous42_128_missing_advapi32_sdk_kat_digest',previous128_missing_advapi32[2]),
            ('previous42_128_missing_userenv_sdk_kat_preimage',previous128_missing_userenv[0]),('previous42_128_missing_userenv_sdk_kat_file_digest',previous128_missing_userenv[1]),('previous42_128_missing_userenv_sdk_kat_digest',previous128_missing_userenv[2]),
            ('current_missing_advapi32_sdk_kat_preimage',current_missing_advapi32[0]),('current_missing_advapi32_sdk_kat_file_digest',current_missing_advapi32[1]),('current_missing_advapi32_sdk_kat_digest',current_missing_advapi32[2]),
            ('current_missing_userenv_sdk_kat_preimage',current_missing_userenv[0]),('current_missing_userenv_sdk_kat_file_digest',current_missing_userenv[1]),('current_missing_userenv_sdk_kat_digest',current_missing_userenv[2]),
            ('previous37_97_sdk_kat_preimage',prior_bytes_image),('previous37_97_sdk_kat_file_digest',prior_bytes_file),('previous37_97_sdk_kat_digest',prior_bytes_digest),
            ('previous_resource_sdk_kat_preimage',prior_resources_image),('previous_resource_sdk_kat_file_digest',prior_resources_file),('previous_resource_sdk_kat_digest',prior_resources_digest),
            ('previous37_new_resources_sdk_kat_preimage',prior_new_resources_image),('previous37_new_resources_sdk_kat_file_digest',prior_new_resources_file),('previous37_new_resources_sdk_kat_digest',prior_new_resources_digest),
            ('previous38_98_sdk_kat_preimage',prior_slice_image),('previous38_98_sdk_kat_file_digest',prior_slice_file),('previous38_98_sdk_kat_digest',prior_slice_digest),
            ('previous38_99_sdk_kat_preimage',prior_native_image),('previous38_99_sdk_kat_file_digest',prior_native_file),('previous38_99_sdk_kat_digest',prior_native_digest)):
        output+='static const uint8_t '+name+'[] = {\n'
        for at in range(0,len(data),16):output+='    '+','.join('0x'+format(x,'02x') for x in data[at:at+16])+',\n'
        output+='};\n'
    # These complete SDK frames use the unchanged declared layout and exact prior semantics.
    for semantic,prior in ((129,previous129),(130,previous130),(131,previous131),(132,previous132),(133,previous133),(134,previous134)):
        for suffix,data in zip(('preimage','file_digest','digest'),prior):
            output+='static const uint8_t previous42_'+str(semantic)+'_sdk_kat_'+suffix+'[] = {\n'
            for at in range(0,len(data),16):output+='    '+','.join('0x'+format(x,'02x') for x in data[at:at+16])+',\n'
            output+='};\n'
    # These predecessors share the same declared ABI layout; only semantic identity differs.
    for semantic in (106,107,109):
        prior_image,prior_file,prior_digest=current_preimage(prior_layout_rows,semantic,30,37,39,37)
        for suffix,data in [('preimage',prior_image),('file_digest',prior_file),('digest',prior_digest)]:
            output+='static const uint8_t previous39_'+str(semantic)+'_sdk_kat_'+suffix+'[] = {\n'
            for at in range(0,len(data),16):output+='    '+','.join('0x'+format(x,'02x') for x in data[at:at+16])+',\n'
            output+='};\n'
    target=Path(__file__).with_name('sdk_current_identity_golden.h')
    if args.write:target.write_text(output,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==output,'current SDK known bytes differ from independent facts'
    print('Current SDK '+str(len(active_rows))+' fields / '+str(len(image))+' byte independent preimage PASS: '+digest.hex())

if __name__=='__main__':main()
