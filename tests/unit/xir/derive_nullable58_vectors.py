"""Frame sum operations from fixed wire facts and retain complete old packets."""
from pathlib import Path
import argparse, hashlib, json, re, struct
from derive_assert_panics_vector import words, op, function, packet
from derive_panic_carrier_vector import packet as scalar_packet
from derive_equal57_migration import migrate, nullable_frame
from derive_test_roles_vectors import upgrade
from derive_semantic61_migration import semantic61_packet
from derive_tuple62_migration import tuple62_packet
from derive_tuple63_migration import tuple63_packet
from derive_role_nullable65_packets import arrays,nullable_header


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
        expected=tuple63_packet(tuple62_packet(semantic61_packet(upgrade(nullable_frame(previous)))))
        path=directory/record['path'];text=path.read_text(encoding='utf-8')
        match=re.search(r'\b'+record['name']+r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S)
        actual=bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',match[1]))
        assert actual==expected
    scalar=scalar_packet(59,23)
    assert product_layout(16).hex()=='a4f36107af3fd82c6fe19e1b99ba2ca1f7f7dd818c75f04e931e26f470abc953'
    assert product_layout(19,25,28).hex()=='40e4c991f0d0f1cbd78ca0d05de11da20c51760d8e4982ec30cc72d1bb1516b4'
    outputs={'xir_checked_scalar59_golden.h':header('checked_scalar59_golden',scalar),
             'xir_nullable_golden.h':header('nullable_none_golden',tuple63_packet(tuple62_packet(semantic61_packet(sum_vector(False)))))+
                                      header('nullable_some_golden',tuple63_packet(tuple62_packet(semantic61_packet(sum_vector(True)))))}
    for name,content in outputs.items():
        actual_text=(directory/name).read_text(encoding='utf-8')
        array_pattern=re.compile(r'const uint8_t (\w+)\[\]=\{(.*?)\};',re.S)
        expected_arrays=list(array_pattern.finditer(content))
        for expected_array in expected_arrays:
            actual_array=next(m for m in array_pattern.finditer(actual_text) if m[1]==expected_array[1])
            decode=lambda text:bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',text))
            assert decode(actual_array[2])==decode(expected_array[2]),expected_array[1]
    identity=(directory/'xir_source_product_identity.h').read_text(encoding='utf-8')
    expected=re.search(r'static const uint8_t expected\[\]=\{(.*?)\};',identity,re.S)
    # Same independent198-byte/43-word layout facts, current Value21/Call26/Program29.
    assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',expected[1]))==product_layout(21,26,29)
    current=nullable_header(arrays(directory/'xir_nullable_golden.h'))
    target=directory/'xir_nullable65_golden.h'
    if args.write:target.write_text(current,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==current
    print(json.dumps({'ordinary_vectors_reframed_with_role_zero':len(records),
        'historical59_nullable_none_sha256':hashlib.sha256(sum_vector(False)).hexdigest(),
        'current63_nullable_none_sha256':hashlib.sha256(tuple63_packet(tuple62_packet(semantic61_packet(sum_vector(False))))).hexdigest(),
        'historical59_nullable_some_sha256':hashlib.sha256(sum_vector(True)).hexdigest(),
        'current63_nullable_some_sha256':hashlib.sha256(tuple63_packet(tuple62_packet(semantic61_packet(sum_vector(True))))).hexdigest()}))


if __name__=='__main__':main()
