"""Independently reframe unchanged ordinary bodies; old56 bytes remain evidence."""
from pathlib import Path
import argparse,hashlib,json,re,struct

def migrate(old):
    assert old[:8]==b'XRCHK\0\0\0' and len(old)>=64
    assert struct.unpack_from('<II',old,8)==(22,56)
    assert struct.unpack_from('<Q',old,24)[0]==len(old)-64
    assert hashlib.sha256(old[:32]+old[64:]).digest()==old[32:64]
    head=bytearray(old[:32]);struct.pack_into('<I',head,12,57)
    return bytes(head)+hashlib.sha256(head+old[64:]).digest()+old[64:]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    directory=Path(__file__).parent;manifest=directory/'equal57_ordinary_vectors.json'
    if not args.write:
        records=json.loads(manifest.read_text(encoding='utf-8'))
        for record in records:
            new=migrate(bytes.fromhex(record['old56_hex']));assert new.hex()==record['current57_hex']
            text=(directory/record['path']).read_text(encoding='utf-8')
            match=re.search(r'\b'+record['name']+r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S)
            assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',match[1]))==new
        from derive_panic_carrier_vector import packet
        old_scalar=packet(56,22);current_scalar=packet(57,22)
        assert old_scalar[32:64].hex()=='ad6f38d1cd3ce90b74cac518abc41bd987cb6809ab4d86f4049260a790101c34'
        scalar=(directory/'xir_checked_scalar57_golden.h').read_text(encoding='utf-8')
        assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',scalar))==current_scalar
        checked=(directory/'test_xir_checked.c').read_text(encoding='utf-8')
        digest=re.search(r'const uint8_t expected_digest\[32\] = \{(.*?)\};',checked,re.S)
        assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',digest[1]))==current_scalar[32:64]
        print(json.dumps({'old56_whole_packets_reproduced':len(records)+1,
                          'current57_whole_packets_verified':len(records)+1}));return
    assert not manifest.exists();records=[]
    for path in sorted(directory.iterdir()):
        if path.suffix not in ('.c','.h') or path.name=='xir_assert_panics_golden.h':continue
        text=path.read_text(encoding='utf-8');edits=[]
        for match in re.finditer(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S):
            old=bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',match[2]))
            if len(old)<64 or old[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',old,8)!=(22,56):continue
            new=migrate(old);records.append({'path':path.name,'name':match[1],'old56_hex':old.hex(),
                'current57_hex':new.hex(),'old56_sha256':hashlib.sha256(old).hexdigest(),'current57_sha256':hashlib.sha256(new).hexdigest(),
                'unchanged_body_sha256':hashlib.sha256(new[64:]).hexdigest()})
            body='\n'+'\n'.join('    '+','.join(f'0x{v:02x}' for v in new[i:i+12])+',' for i in range(0,len(new),12))+'\n'
            edits.append((match.start(2),match.end(2),body))
        for first,last,new in reversed(edits):text=text[:first]+new+text[last:]
        if edits:path.write_text(text,encoding='utf-8',newline='\n')
    manifest.write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'migrated':len(records),'paths':sorted(set(r['path'] for r in records))},indent=2))

if __name__=='__main__':main()
