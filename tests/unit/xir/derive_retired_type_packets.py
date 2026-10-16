"""Retain full authenticated historical inputs independently of current ABI macros."""
from pathlib import Path
import argparse,hashlib,json,re,struct
from derive_construction72_vectors import header
ROOT=Path(__file__).resolve().parent
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    rows=[]
    for index,row in enumerate(json.loads((ROOT/'retired_type_packet_inputs.json').read_text())):
        raw=(ROOT/row['source']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256'],row['source']
        for match in re.finditer(r'static const uint8_t (\w+)\[\] ?= ?\{(.*?)\};',raw.decode(),re.S):
            if 'models' in row and match[1] not in row['models']:continue
            packet=bytes(int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            if packet[:8]!=b'XRCHK\0\0\0':continue
            assert len(packet)>=64 and struct.unpack_from('<Q',packet,24)[0]==len(packet)-64
            assert hashlib.sha256(packet[:32]+packet[64:]).digest()==packet[32:64]
            assert struct.unpack_from('<II',packet,8) in ((26,71),(28,73),(29,75),(28,74),(28,75),(29,76),(30,77),(31,78),(32,79))
            rows.append(('retired_'+str(index)+'_'+match[1],packet))
    assert len(rows)==65 and len({name for name,_ in rows})==65
    header(ROOT/'retired_type_packets.h','RETIRED_TYPE_PACKETS_H',rows,args.write)
    print('Full retired declaration/hash packet bytes preserved:',len(rows))
if __name__=='__main__':main()
