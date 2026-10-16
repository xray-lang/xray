"""Independently frame Unit-value semantic packets from complete retired fields.

No product writer, reader, compiler or executable supplies these bytes. Every
retired packet and its complete field digest are authenticated before framing.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct

PINS = {'compile_owner/checked_sizing75_golden.h': '590347ea2c51b255d9c6bb5750ce9b4ec1b4f81d117811d51abdf50e43b14c7f', 'xir_assert_panics75_golden.h': 'a248c2d7264d46768e10bf0f8d4c8e5f617d5cb9ec110a769d8f81677c4e2451', 'xir_checked_range75_golden.h': '6c2d258dc716bba4b54867fb71b9d32a6d7f4a222ed1cb417f60e71cfa4a149d', 'xir_checked_scalar75_golden.h': '182320babbe7fb0658c2f6c7ec03afefda44e379d984b94ec4a29b3c3bcc736f', 'xir_error_marker75_golden.h': 'c0ef1ff0642bdff07fea29ee75679608dae5f2e8cf1e2a45cb183f09220ba30d', 'xir_generic_method75_golden.h': '87de5efe29e2ad559da90765cdfb7a7ad00e3cc38a40b4d8fb7c99d3f8618952', 'xir_rune75_golden.h': 'da2ad7380ff5f1163e781a565aa129f4eb634c3bf5b7b4600949a1dd9fbae073'}

def arrays(text):
    return list(re.finditer(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',text,re.S))
def octets(match):
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
def render(name,data):
    return 'static const uint8_t '+name+'[] = {\n'+''.join('    '+', '.join('0x%02x'%b for b in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};'
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    root=Path(__file__).resolve().parent;records=[]
    for rel,pin in PINS.items():
        source=root/rel;text=source.read_text(encoding='utf-8');assert hashlib.sha256(text.encode()).hexdigest()==pin,rel
        replacements={};digests={}
        for match in arrays(text):
            data=octets(match)
            if data[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',data,8)!=(29,75):continue
            assert struct.unpack_from('<Q',data,24)[0]==len(data)-64
            assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]
            prefix=data[:12]+struct.pack('<I',76)+data[16:32]
            digest=hashlib.sha256(prefix+data[64:]).digest();new=prefix+digest+data[64:]
            replacements[match[1]]=new;digests[data[32:64]]=digest
            assert new[64:]==data[64:] and new[:12]==data[:12] and new[16:32]==data[16:32]
            records.append(dict(symbol=match[1].replace('75','76'),bytes=len(new),retired_sha256=hashlib.sha256(data).hexdigest(),sha256=hashlib.sha256(new).hexdigest(),body_unchanged=True))
        def row(match):
            old=octets(match);new=replacements.get(match[1],digests.get(old,old))
            return render(match[1].replace('75','76'),new)
        output=re.sub(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',row,text,flags=re.S)
        output=re.sub(r'\b[A-Za-z_]\w*75\w*\b',lambda m:m[0].replace('75','76'),output)
        output=output.replace('CONTRACT == 75u','CONTRACT == 76u')
        target=source.with_name(source.name.replace('75','76'))
        if args.write:target.write_bytes(output.encode())
        else:assert target.read_text(encoding='utf-8')==output,str(target)
    assert records and len(PINS)==7
    from unit_values.derive_unit_wire import verify_unit_packets
    verify_unit_packets()
    print(json.dumps(dict(records=records,product_writer_used=False,retired_inputs_unchanged=True),indent=2))
if __name__=='__main__':main()
