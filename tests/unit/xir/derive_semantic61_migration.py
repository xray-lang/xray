"""Independent same-body semantic61 oracle; never imports the Xray producer."""
from pathlib import Path
import argparse,hashlib,json,re,struct
def semantic61_packet(previous):
    assert previous[:8]==b'XRCHK\0\0\0' and len(previous)>=64
    assert struct.unpack_from('<II',previous,8) in ((23,59),(23,60))
    assert struct.unpack_from('<Q',previous,24)[0]==len(previous)-64
    assert hashlib.sha256(previous[:32]+previous[64:]).digest()==previous[32:64]
    header=bytearray(previous[:32]);struct.pack_into('<I',header,12,61)
    return bytes(header)+hashlib.sha256(header+previous[64:]).digest()+previous[64:]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path);args=parser.parse_args()
    directory=Path(__file__).parent;root=directory.parents[2]
    history=json.loads((directory/'semantic60_packet_history.json').read_text(encoding='utf8'))
    assert (history['wire'],history['semantic'])==(23,60) and len(history['packets'])==23
    expression=re.compile(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',re.S)
    if args.output:args.output.mkdir(parents=True,exist_ok=True)
    for i,row in enumerate(history['packets']):
        old=bytes.fromhex(row['old60_hex']);current=semantic61_packet(old)
        assert len(old)==row['length'] and hashlib.sha256(old).hexdigest()==row['old60_sha256']
        assert hashlib.sha256(old[64:]).hexdigest()==row['body_sha256']
        assert current[64:]==old[64:] and hashlib.sha256(current).hexdigest()==row['current61_sha256']
        match=next(m for m in expression.finditer((root/row['path']).read_text(encoding='utf8')) if m[1]==row['name'])
        from derive_tuple62_migration import tuple62_packet
        assert bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})\b',match[2]))==tuple62_packet(current)
        if args.output:
            (args.output/f'{i:02d}-old60.chk').write_bytes(old)
            (args.output/f'{i:02d}-current61.chk').write_bytes(current)
    print('23 complete old60 packets preserved; independent historical same-body61 and current24/62 bytes verified; reader qualification separate')
if __name__=='__main__':main()
