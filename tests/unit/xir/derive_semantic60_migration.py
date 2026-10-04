"""Independent historical60 projection; historical packet bytes are immutable facts.

This module never imports an Xray producer, writer, ABI macro, or build artifact.
It is a test oracle, not a production reader or compatibility path.
"""
from pathlib import Path
import argparse, hashlib, json, re, struct

def semantic60_packet(previous):
    assert previous[:8] == b'XRCHK\0\0\0' and len(previous) >= 64
    assert struct.unpack_from('<II', previous, 8) == (23, 59)
    assert struct.unpack_from('<Q', previous, 24)[0] == len(previous) - 64
    assert hashlib.sha256(previous[:32] + previous[64:]).digest() == previous[32:64]
    head = bytearray(previous[:32]); struct.pack_into('<I', head, 12, 60)
    return bytes(head) + hashlib.sha256(head + previous[64:]).digest() + previous[64:]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args=parser.parse_args()
    directory=Path(__file__).parent
    history=json.loads((directory/'semantic59_packet_history.json').read_text(encoding='utf-8'))
    assert (history['wire'],history['semantic']) == (23,59)
    records=history['packets'];assert len(records)==19
    expression=re.compile(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',re.S)
    if args.output:args.output.mkdir(parents=True,exist_ok=True)
    for index,row in enumerate(records):
        old=bytes.fromhex(row['packet_hex']);current=semantic60_packet(old)
        assert len(old)==row['length'] and hashlib.sha256(old).hexdigest()==row['sha256']
        assert hashlib.sha256(old[64:]).hexdigest()==row['body_sha256']
        assert current[64:]==old[64:] and current[:12]==old[:12] and current[16:32]==old[16:32]
        assert hashlib.sha256(current).hexdigest()==row['current60_sha256']
        complete=json.loads((directory/'semantic60_packet_history.json').read_text(encoding='utf-8'))['packets']
        preserved=next(p for p in complete if p['path']==row['current_path'] and p['name']==row['current_name'])
        assert bytes.fromhex(preserved['old60_hex'])==current,(row['path'],row['name'])
        if row['name']=='checked_scalar59_golden':
            historical=(directory/'xir_checked_scalar59_golden.h').read_text(encoding='utf-8')
            old_match=next(m for m in expression.finditer(historical) if m[1]==row['name'])
            assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}\b',old_match[2]))==old
        if args.output:
            (args.output/f'{index:02d}-old59.chk').write_bytes(old)
            (args.output/f'{index:02d}-current60.chk').write_bytes(current)
    print('19 complete semantic59 inputs preserved; independent23/60 same-body packets verified; real reader qualification remains separate')

if __name__=='__main__':main()
