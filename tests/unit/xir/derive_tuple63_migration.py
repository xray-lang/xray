"""Strict current63 projection; complete62 bodies and identities remain evidence."""
from pathlib import Path
import hashlib,json,re,struct

def tuple63_packet(previous):
    assert previous[:8]==b'XRCHK\0\0\0' and len(previous)>=64
    assert struct.unpack_from('<4I',previous,8)==(24,62,2,0)
    assert struct.unpack_from('<Q',previous,24)[0]==len(previous)-64
    assert hashlib.sha256(previous[:32]+previous[64:]).digest()==previous[32:64]
    header=bytearray(previous[:32]);struct.pack_into('<I',header,12,63)
    return bytes(header)+hashlib.sha256(header+previous[64:]).digest()+previous[64:]

def main():
    directory=Path(__file__).parent;root=directory.parents[2]
    history=json.loads((directory/'tuple62_packet_history.json').read_text(encoding='utf-8'))
    assert history['base']=='4e10d694e161c3ee344dfdb92e6c704f83397f7e' and len(history['packets'])==25
    expression=re.compile(r'static\s+const\s+(?:uint8_t|unsigned\s+char)\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',re.S)
    for row in history['packets']:
        old=bytes.fromhex(row['old62_hex']);current=tuple63_packet(old)
        assert len(old)==row['length'] and hashlib.sha256(old).hexdigest()==row['old62_sha256']
        assert hashlib.sha256(old[64:]).hexdigest()==row['body_sha256'] and current[64:]==old[64:]
        assert hashlib.sha256(current).hexdigest()==row['current63_sha256']
        if row['name']=='tuple_24_62':path=directory/'tuple_24_63.inc.c';name='tuple_24_63'
        else:path=root/row['path'];name=row['name']
        match=next(m for m in expression.finditer(path.read_text(encoding='utf-8')) if m[1]==name)
        assert bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b',match[2]))==current
    print('25 complete62 identities/digests preserved; current63 same body and literal bytes PASS')

if __name__=='__main__':main()
