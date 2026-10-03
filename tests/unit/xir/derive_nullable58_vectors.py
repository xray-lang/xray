"""Frame sum operations from fixed wire facts and retain complete old packets."""
from pathlib import Path
import argparse, hashlib, json, re, struct
from derive_assert_panics_vector import words, op, function, packet
from derive_panic_carrier_vector import packet as scalar_packet
from derive_equal57_migration import migrate, nullable_frame
from derive_test_roles_vectors import upgrade


def sum_vector(some):
    instructions = [op(2,2,immediate=7),op(119,256)] if some else [op(118,256)]
    instructions += [op(25,left=int(some))]
    body = words(0,1,0) + function('n',[],256,[(0,len(instructions),0,0)],instructions)
    # Empty operands were emitted by function(); no generics, one unary node,
    # no nominal or interface declarations, no defaults and no provenance.
    body += words(0,1,0,0,5,0,2,0,0)
    return packet(body,23,59)


def header(name, data):
    return ('/* Independently framed little-endian sum packet. */\n'
            f'static const uint8_t {name}[]={{\n' +
            '\n'.join('    '+','.join(f'0x{v:02x}' for v in data[i:i+12])+','
                      for i in range(0,len(data),12))+'\n};\n')


def product_layout(value_abi,call_abi=21,program_abi=26):
    # identity(string)->string, init()->Unit, entry()->i64. Each RETURN has
    # a logical Unit slot at UINT32_MAX; only the string parameter owns storage.
    data=b'xray:xir-lowered-layout:v1'+words(1,1,value_abi,call_abi,program_abi,2,3,1)
    data+=words(0,1,2,8,16,8,1,0,0,0,0xffffffff,16,8,0)
    data+=words(1,0,1,0,16,8,0,0,0,0xffffffff)
    data+=words(2,0,2,8,16,8,0,0,0,0,0xffffffff)
    assert len(data)==198
    return hashlib.sha256(data).digest()


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    directory=Path(__file__).parent
    records=json.loads((directory/'equal57_ordinary_vectors.json').read_text(encoding='utf-8'))
    for record in records:
        previous=migrate(bytes.fromhex(record['old56_hex']))
        assert previous.hex()==record['current57_hex']
        expected=upgrade(nullable_frame(previous))
        path=directory/record['path'];text=path.read_text(encoding='utf-8')
        match=re.search(r'\b'+record['name']+r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S)
        actual=bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',match[1]))
        if args.write:
            assert actual==expected
            content=header(record['name'],expected).split('{',1)[1].split('};',1)[0]
            path.write_text(text[:match.start(1)]+content+text[match.end(1):],encoding='utf-8')
        else: assert actual==expected
    scalar=scalar_packet(59,23)
    assert product_layout(16).hex()=='a4f36107af3fd82c6fe19e1b99ba2ca1f7f7dd818c75f04e931e26f470abc953'
    outputs={'xir_checked_scalar59_golden.h':header('checked_scalar59_golden',scalar),
             'xir_nullable_golden.h':header('nullable_none_golden',sum_vector(False))+
                                      header('nullable_some_golden',sum_vector(True))}
    for name,content in outputs.items():
        if args.write:(directory/name).write_text(content,encoding='utf-8')
        else:assert (directory/name).read_text(encoding='utf-8')==content
    if args.write:
        checked=directory/'test_xir_checked.c';text=checked.read_text(encoding='utf-8')
        digest=re.search(r'const uint8_t expected_digest\[32\] = \{(.*?)\};',text,re.S)
        content='\n        '+', '.join(f'0x{v:02x}' for v in scalar[32:64])+'\n    '
        checked.write_text(text[:digest.start(1)]+content+text[digest.end(1):],encoding='utf-8')
        identity=directory/'xir_source_product_identity.h';text=identity.read_text(encoding='utf-8')
        expected=re.search(r'static const uint8_t expected\[\]=\{(.*?)\};',text,re.S)
        content=','.join(f'0x{v:02x}' for v in product_layout(18,22,28))
        identity.write_text(text[:expected.start(1)]+content+text[expected.end(1):],encoding='utf-8')
    else:
        identity=(directory/'xir_source_product_identity.h').read_text(encoding='utf-8')
        expected=re.search(r'static const uint8_t expected\[\]=\{(.*?)\};',identity,re.S)
        assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',expected[1]))==product_layout(18,23,28)
    print(json.dumps({'ordinary_vectors_reframed_with_role_zero':len(records),
        'nullable_none_sha256':hashlib.sha256(sum_vector(False)).hexdigest(),
        'nullable_some_sha256':hashlib.sha256(sum_vector(True)).hexdigest()}))


if __name__=='__main__':main()
