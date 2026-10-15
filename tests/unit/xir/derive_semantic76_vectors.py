"""Independently frame Unit-value semantic packets from complete retired fields.

No product writer, reader, compiler or executable supplies these bytes. Every
retired packet and its complete field digest are authenticated before framing.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct,tempfile

PINS = {'compile_owner/checked_sizing75_golden.h': 'a53a0e36812037fb89256aa6c16adb551afe74980e9dd70b4415e1d4926c810d', 'xir_assert_panics75_golden.h': '3ab1efc6fdcb2dabac891a9f19a16bc08a7224837b136163347a78e5b1c56cd1', 'xir_checked_range75_golden.h': 'bc3be7d74e0ff26d593a24d6d9beffd75028bf045c8848b539d0007c563cdb20', 'xir_checked_scalar75_golden.h': 'dccffe75da5b2382c7609a470cb426298021499fc01512e55023da6687bc4740', 'xir_error_marker75_golden.h': 'c818ac3af184d1d15d91a3094ab8ff627e6b04c003bb6db989aeffd574152665', 'xir_generic_method75_golden.h': '874c5822e30bc8ccb44b3cc674782fe3fc609b8b3fb02a99648d16f6a91ca8ae', 'xir_rune75_golden.h': '1a8a820ac5f615d0692f49d3d5a85e4bd7bc0acbf9a348ad322bb90467c0c37b', 'xir_hash75_golden.h': '306cc8c77be0effd4e4fc3bd2a6c529dd250e792e3810f5e5b1c25854f35f500', 'xir_nullable75_golden.h': 'd42023e82dd0e20fa715ed6689c1b76d0411c49c69992a04484b608f8320cce3', 'xir_nested_nullable75_golden.h': '05477399d322efa71dce9e24efefb22724c4deafe15ad612dcc0e42d3772bdcd'}

def arrays(text):
    return list(re.finditer(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',text,re.S))
def octets(match):
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
def render(name,data):
    return 'static const uint8_t '+name+'[] = {\n'+''.join('    '+', '.join('0x%02x'%b for b in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};'
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    root=Path(__file__).resolve().parent;records=[];retired=[]
    for rel,pin in PINS.items():
        source=root/rel;text=source.read_text(encoding='utf-8');assert hashlib.sha256(text.encode()).hexdigest()==pin,rel
        replacements={};digests={}
        for match in arrays(text):
            data=octets(match)
            if data[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',data,8)!=(29,75):continue
            assert struct.unpack_from('<Q',data,24)[0]==len(data)-64
            assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]
            retired.append(('local_previous75_'+match[1],data))
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
    assert records and len(PINS)==10
    from derive_construction72_vectors import header
    with tempfile.TemporaryDirectory(prefix='retired75-fields-') as temporary:
        name='xir_local_previous75_golden.h';guard='XIR_LOCAL_PREVIOUS75_GOLDEN_H'
        generated=Path(temporary)/name
        header(generated,guard,retired,True)
        previous=generated.read_text(encoding='utf-8').replace('#include <stdint.h>', '#include <stdint.h>\n#include <stddef.h>')
    table='typedef struct LocalPrevious75Vector { const uint8_t *bytes; size_t length; } LocalPrevious75Vector;\n'
    table+='static const LocalPrevious75Vector local_previous75_vectors[] = {\n'
    table+=''.join('    {'+name+',sizeof('+name+')},\n' for name,_ in retired)+'};\n'
    previous=previous.replace('#endif // '+guard,table+'#endif // '+guard)
    if args.write:(root/name).write_text(previous,encoding='utf-8',newline='\n')
    else:assert (root/name).read_text(encoding='utf-8')==previous
    from unit_values.derive_unit_wire import verify_unit_packets
    verify_unit_packets()
    print(json.dumps(dict(records=records,product_writer_used=False,retired_inputs_unchanged=True),indent=2))
if __name__=='__main__':main()
