"""Independent current field framing and complete immutable parent controls."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

ROOT=Path(__file__).resolve().parent


def words(*values):
    return struct.pack('<'+'I'*len(values),*values)


def channel_packet(element, semantic=91):
    name=b'channelTypes'
    function=words(len(name))+name+words(2,256,257,0,1,0,1,0,0,1)
    instruction=words(33,0,0,0,0,0)+struct.pack('<q',0)+words(0,0)
    # Empty generics, two Channel signatures, empty nominal/interface tables.
    body=words(0,1,0)+function+instruction+words(0,0,2,0,0,14,0,element,14,0,3)
    # Construction rows, materializations, CTFE sites/bindings, defaults, provenance.
    body+=words(0,0,0,0,0,0)
    prefix=b'XRCHK\0\0\0'+struct.pack('<4IQ',32,semantic,2,0,len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    model=json.loads((ROOT/'shared_parent_checked_inputs.json').read_text())
    assert [p['commit'] for p in model['parents']]==[
        '5b7af8a3d922ceabafdc857279f4bbd167834b99','6d6754d9b77e8799658113a9c2bad2e561f8f2ae',
        '52d63f5cdd3b1baa02a5f5241075eecad0adbaa3','d9b4ba94ce7694e456d82ec36c2449dd3d8ba96c',
        'd4d4faf5d84dd729a3129e7c0c8740230cfd43e8']
    assert len(model['vectors'])==1284
    container=b'XRPRNT01'+words(len(model['vectors']))
    for row in model['vectors']:
        data=bytes.fromhex(row['hex'])
        assert len(data)==row['bytes'] and hashlib.sha256(data).hexdigest()==row['sha256']
        assert data[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<Q',data,24)[0]==len(data)-64
        assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]
        container+=words(len(data))+bytes.fromhex(row['sha256'])+data
    header='/* Independent current Channel signature field model. */\n'
    previous=(ROOT/'shared_previous90_checked.h').read_text()
    for name,element in [('shared_channel_int',2),('shared_channel_unit',0)]:
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',previous,re.S)
        assert match and bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})',match[1]))==channel_packet(element,90)
        data=channel_packet(element)
        header+='static const uint8_t '+name+'[] = {\n'
        header+=''.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+16])+',\n' for i in range(0,len(data),16))
        header+='};\n'
    expected={'shared_parent_packets.bin':container,'shared_current_checked.h':header.encode()}
    for filename,data in expected.items():
        path=ROOT/filename
        if args.write:path.write_bytes(data)
        else:assert path.read_bytes()==data,filename
    print(json.dumps({'fixed_parents':5,'complete_unique_parent_packets':1284,'complete_packet_bytes':804442,
                      'container_sha256':hashlib.sha256(container).hexdigest(),'current':[32,91],
                      'expected_source':'explicit function, instruction, type and empty metadata fields; no writer output'}))


if __name__=='__main__':main()
