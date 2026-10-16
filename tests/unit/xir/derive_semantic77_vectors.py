"""Independently frame zero-slot vectors and preserve complete retired packets.

The listed models contain no slot records, including recursive construction
sources. Their complete authenticated bodies therefore stay byte-identical.
Exported and private slot records have a separate explicit-field oracle.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct

ROOT=Path(__file__).resolve().parent
def arrays(text):
    return re.finditer(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',text,re.S)
def octets(match):return bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
def render(name,data):
    return 'static const uint8_t '+name+'[] = {\n'+''.join('    '+','.join('0x%02x'%v for v in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};'
def frame(packet):
    assert packet[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',packet,8)==(29,76)
    assert struct.unpack_from('<Q',packet,24)[0]==len(packet)-64
    assert hashlib.sha256(packet[:32]+packet[64:]).digest()==packet[32:64]
    header=packet[:8]+struct.pack('<II',30,77)+packet[16:32]
    return header+hashlib.sha256(header+packet[64:]).digest()+packet[64:]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    registry=json.loads((ROOT/'semantic77_inputs.json').read_text(encoding='utf-8'))
    count=0
    for row in registry:
        source=ROOT/row['source'];raw=source.read_bytes()
        assert hashlib.sha256(raw).hexdigest()==row['sha256'],row['source']
        text=raw.decode('utf-8').replace('\r\n','\n');digests={};packets={}
        for match in arrays(text):
            old=octets(match)
            if old[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',old,8)!=(29,76):continue
            assert match[1] in row['zero_slot_models']
            new=frame(old);packets[match[1]]=new;digests[old[32:64]]=new[32:64];count+=1
        assert len(packets)==len(row['zero_slot_models'])
        def replace(match):
            old=octets(match)
            return render(match[1].replace('76','77'),packets.get(match[1],digests.get(old,old)))
        output=re.sub(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',replace,text,flags=re.S)
        output=re.sub(r'\b[A-Za-z_]\w*76\w*\b',lambda m:m[0].replace('76','77'),output)
        output=output.replace('SCHEMA == 29u','SCHEMA == 30u').replace('CONTRACT == 76u','CONTRACT == 77u')
        if row['source']=='unit_values/unit_wire_golden.h':
            old=next(octets(m) for m in arrays(text) if m[1]=='unit76_packet')
            output+='\n'+render('previous76_packet',old)+'\n'
        target=ROOT/row['target']
        if args.write:target.write_bytes(output.encode())
        else:assert target.read_text(encoding='utf-8')==output,row['target']
    assert count==14
    print(json.dumps({'independent_zero_slot_frames':count,'retired_files':len(registry),'product_writer_used':False}))
if __name__=='__main__':main()
