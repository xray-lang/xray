"""Reproduce one static SDK preimage from fixed bytes and natural C layout facts."""
from pathlib import Path
import argparse, hashlib, json, struct, sys, tempfile

root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'scripts'))
from derive_xir_sdk_abi import prepare


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='xir-sdk-kat-') as temporary:
        facts=Path(temporary)/'facts';prepare(facts)
        rows=json.loads((facts/'EXPECTED.json').read_text())['rows'];assert len(rows)==233
    word=lambda value:struct.pack('<I',value)
    def text(value):
        value=value.encode();return word(len(value))+value
    prefix=[2,23,59,18,23,28,1,1,1,11,2,0,1,0,3,1,1]
    image=b'xray:xir-runtime-sdk:v1'+struct.pack('<17I',*prefix)
    image+=text('x86_64-windows-msvc')+text('xray:xir-runtime-abi-measurements:v1')
    image+=text('xray:xir-runtime-recipe:windows-x86_64-hosted:v1')+word(233)
    image+=b''.join(struct.pack('<II',row['id'],row['value']) for row in rows)
    file_digest=hashlib.sha256(b'hi').digest()
    image+=word(1)+text('lib/test.lib')+word(5)+struct.pack('<Q',2)+file_digest
    image+=word(1)+text('kernel32');digest=hashlib.sha256(image).digest()
    output='/* Independent fixed bytes and natural Windows C layout facts. */\n'
    for name,data in (('sdk_kat_preimage',image),('sdk_kat_file_digest',file_digest),('sdk_kat_digest',digest)):
        output+='static const uint8_t '+name+'[] = {\n'
        for at in range(0,len(data),16):output+='    '+','.join('0x'+format(x,'02x') for x in data[at:at+16])+',\n'
        output+='};\n'
    target=Path(__file__).with_name('sdk_identity_golden.h')
    if args.write:target.write_text(output,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==output,'static SDK known bytes differ from independent facts'
    print('SDK independent '+str(len(image))+' byte preimage PASS: '+digest.hex())


if __name__=='__main__':main()
