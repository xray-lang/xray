"""Encode the combined policy and every complete parent policy from fixed fields."""
from pathlib import Path
import argparse
import hashlib
import re
import struct

ROOT = Path(__file__).resolve().parent


def arrays():
    functions = [[0,1,2,8,16,8,1,0,0,0,0xffffffff,16,8,0],
                 [1,0,1,0,16,8,0,0,0,0xffffffff],
                 [2,0,2,8,16,8,0,0,0,0,0xffffffff]]
    layouts = {}
    for value, call, program, suffix, preimage in [
        (24,29,30,'value24','layout_current24_preimage'),
        (24,29,31,'program31','layout_program31_preimage'),
        (29,33,34,'value29','layout_current29_preimage'),
        (29,33,35,'program35','layout_program35_preimage'),
    ]:
        words = [1,1,value,call,program,2,3,1] + sum(functions, [])
        assert len(words) == 43
        data = b'xray:xir-lowered-layout:v1' + struct.pack('<43I', *words)
        layouts[preimage] = data
        layouts['product_layout_' + suffix + '_digest'] = hashlib.sha256(data).digest()
    policies = {}
    for prefix in ['linked', 'original']:
        text = (prefix + '_source').encode('ascii')
        for wire, semantic, value, call, program, suffix in [
            (28,73,24,29,30,'_program30'), (28,73,24,29,31,'_value24'),
            (30,75,24,29,31,'75'), (28,76,29,33,34,'_value29'),
            (28,76,29,33,35,'_program35'), (30,77,29,33,35,'77'),
        ]:
            data = b'xray:xir-c11-codegen-policy:v1' + struct.pack('<7I',1,wire,semantic,value,call,program,len(text)) + text
            policies[prefix + '_policy' + suffix + '_preimage'] = data
            policies[prefix + '_policy' + suffix] = hashlib.sha256(data).digest()
    return {'xir_source_product_identity.h': layouts, 'xir_native_projection_expectations.h': policies}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    for filename, values in arrays().items():
        path = ROOT / filename
        source = path.read_text(encoding='utf-8')
        for name, expected in values.items():
            pattern = r'(static const uint8_t ' + name + r'\[\]\s*=\s*\{)(.*?)(\};)'
            match = re.search(pattern, source, re.S)
            if args.write:
                rows = '\n' + ''.join('    ' + ','.join(f'0x{byte:02x}' for byte in expected[i:i+12]) + ',\n'
                                      for i in range(0,len(expected),12))
                if match:
                    source = source[:match.start(2)] + rows + source[match.end(2):]
                else:
                    source = source.replace('static void ', 'static const uint8_t ' + name + '[] = {' + rows + '};\n\nstatic void ', 1)
            else:
                assert match, (filename, name)
                actual = bytes(int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
                assert actual == expected, (filename, name)
        if args.write:
            path.write_text(source,encoding='utf-8',newline='\n')
    print('Independent combined30/77 policy and complete parent layout/policy frames PASS')


if __name__ == '__main__':
    main()
