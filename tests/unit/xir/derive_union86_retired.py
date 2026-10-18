"""Preserve complete retired Checked bytes and enumerate rejecting receivers."""
from pathlib import Path
import argparse,hashlib,json,re,struct
HERE=Path(__file__).resolve().parent
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    packets={}
    for path in sorted(HERE.rglob('*golden*.h')):
        for match in re.finditer(r'static const uint8_t (\w+)\[\d*\]\s*=\s*\{(.*?)\};',path.read_text('utf-8'),re.S):
            data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            if not data.startswith(b'XRCHK\0\0\0') or len(data)<64:continue
            wire,semantic=struct.unpack_from('<II',data,8)
            if (wire,semantic)==(30,86):continue
            sha=hashlib.sha256(data).hexdigest();relative='retired_fixed_inputs/packets/'+sha+'.bin'
            row=packets.setdefault(sha,dict(path=relative,bytes=len(data),wire=wire,semantic=semantic,sources=[]))
            row['sources'].append(dict(path=path.relative_to(HERE).as_posix(),symbol=match[1]))
            target=HERE/relative
            if args.write:target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
            else:assert target.read_bytes()==data
    manifest=json.dumps(packets,indent=2)+'\n'
    header='/* Complete immutable retired packets; generated independently of product writers. */\n'
    header+='static const RetiredPacket retired_packets[]={\n'
    outputs=[('retired_fixed_inputs/packets.json',manifest)]
    ordered=sorted(packets.items())
    for first in range(0,len(ordered),400):
        name=f'union86_retired_table_part{first//400}.h'
        header+='#include "'+name+'"\n'
        part=''.join('    {"'+row['path']+'",'+str(row['bytes'])+',"'+sha+'"},\n' for sha,row in ordered[first:first+400])
        outputs.append((name,part))
    header+='};\n';outputs.append(('union86_retired_table.h',header))
    for name,text in outputs:
        target=HERE/name
        if args.write:target.write_text(text,encoding='utf-8',newline='\n')
        else:assert target.read_text('utf-8')==text
    print(f'Retired complete packet controls: {len(packets)} unique vectors')
if __name__=='__main__':main()
