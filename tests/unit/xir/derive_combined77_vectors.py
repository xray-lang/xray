"""Reframe complete CTFE field models under the combined identity, never a writer."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile


def arrays(text):
    return {name: bytes(int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{2})',body))
            for name, body in re.findall(r'static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};',text,re.S)}


def frame(body):
    head = b'XRCHK\0\0\0' + struct.pack('<IIIIQ',30,77,2,0,len(body))
    return head + hashlib.sha256(head + body).digest() + body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write',action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    test = root / 'tests/unit/xir'
    records = []
    with tempfile.TemporaryDirectory(prefix='independent-ctfe75-models-') as temporary:
        stage = Path(temporary)
        (stage / 'tests/unit/xir/compile_owner').mkdir(parents=True)
        for name in ['derive_comptime75_vectors.py','derive_root_comptime75_vectors.py',
                     'derive_binding75_vector.py','derive_library3075_vectors.py']:
            subprocess.run([sys.executable,'-X','utf8',str(test/name),'--source-root',str(root),
                            '--output-root',str(stage),'--write'],check=True,stdout=subprocess.DEVNULL)
        for path in sorted((stage/'tests/unit/xir').rglob('*.h')):
            relative = path.relative_to(stage/'tests/unit/xir')
            old_text = path.read_text(encoding='utf-8')
            data = arrays(old_text)
            expected_file = test / relative
            if relative.name == 'xir_assert_panics75_golden.h':
                expected_file = test / 'xir_assert_panics_ctfe75_golden.h'
                expected = {name.replace('_ctfe75','75'): value for name,value in arrays(expected_file.read_text()).items()}
            else:
                expected = arrays(expected_file.read_text(encoding='utf-8'))
            assert data == expected, ('complete parent model changed',relative)
            current = {name: frame(value[64:]) if value.startswith(b'XRCHK\0\0\0') and
                       struct.unpack_from('<II',value,8)==(30,75) else value for name,value in data.items()}
            for name in current:
                if name.endswith('_digest') and name[:-7]+'_golden' in current:
                    current[name] = current[name[:-7]+'_golden'][32:64]
            def declaration(match):
                name = match.group(1)
                rows = ''.join('    '+','.join(f'0x{byte:02x}' for byte in current[name][i:i+12])+',\n'
                               for i in range(0,len(current[name]),12))
                return 'static const uint8_t '+name+'[] = {\n'+rows+'};'
            text = re.sub(r'static const uint8_t (\w+)\[\]\s*=\s*\{.*?\};',declaration,old_text,flags=re.S)
            text = re.sub(r'\b[A-Za-z_]\w*\b',lambda match: match[0].replace('75','77'),text)
            destination = test / str(relative).replace('75','77')
            if args.write:
                destination.write_text(text,encoding='utf-8',newline='\n')
            else:
                assert destination.read_text(encoding='utf-8')==text,destination
            records.append({'header':relative.as_posix(),'complete_parent_vectors_verified':len(data),
                            'current_vectors':sum(value.startswith(b'XRCHK\0\0\0') for value in current.values())})
    print(json.dumps({'wire':30,'semantic':77,'product_writer_used':False,'records':records},indent=2))


if __name__=='__main__':
    main()
