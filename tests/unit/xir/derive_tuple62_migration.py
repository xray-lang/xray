"""Strict 24/62 projection for unchanged original-kind 23/61 bodies."""
import hashlib,struct

def tuple62_packet(previous):
    assert previous[:8]==b'XRCHK\0\0\0' and len(previous)>=64
    assert struct.unpack_from('<4I',previous,8)==(23,61,2,0)
    assert struct.unpack_from('<Q',previous,24)[0]==len(previous)-64
    assert hashlib.sha256(previous[:32]+previous[64:]).digest()==previous[32:64]
    header=bytearray(previous[:32]);struct.pack_into('<II',header,8,24,62)
    return bytes(header)+hashlib.sha256(header+previous[64:]).digest()+previous[64:]

def main():
    from pathlib import Path
    import json,re
    from derive_semantic61_migration import semantic61_packet
    directory=Path(__file__).parent;root=directory.parents[2]
    history=json.loads((directory/'semantic60_packet_history.json').read_text(encoding='utf-8'))
    rows=[row for row in history['packets'] if row['path']=='tests/unit/xir/compile_owner/checked_sizing_cases.h']
    assert len(rows)==3
    expression=re.compile(r'static const uint8_t (\w+)\[[^\]]*\] = \{(.*?)\};',re.S)
    source=(root/rows[0]['path']).read_text(encoding='utf-8')
    for row in rows:
        previous=semantic61_packet(bytes.fromhex(row['old60_hex']))
        assert hashlib.sha256(previous).hexdigest()==row['current61_sha256']
        source=(root/row['path']).read_text(encoding='utf-8')
        match=next(m for m in expression.finditer(source) if m[1]==row['name'])
        actual=bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})\b',match[2]))
        from derive_tuple63_migration import tuple63_packet
        expected=tuple63_packet(tuple62_packet(previous))
        assert expected[64:]==previous[64:] and actual==expected
    print('23 complete current KATs: preserved old histories, same original body, strict schema24/semantic63 bytes PASS')
if __name__=='__main__':main()
