"""Frame independent predecessor fields under the typed storage-view contract.

Only the Checked semantic header changes for existing operations. Their complete
accepted and rejected bodies remain immutable, including malformed type cases.
The product writer and runtime digests are never used to derive expectations.
"""
from pathlib import Path
import argparse, hashlib, re, struct
from derive_callable71_checked_vectors import encode_body
from derive_construction72_vectors import header

HERE = Path(__file__).resolve().parent

def arrays(text):
    return [(m, bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m[2])))
        for m in re.finditer(r'static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};', text, re.S)]

def frame(body, semantic):
    prefix = b'XRCHK\0\0\0' + struct.pack('<4IQ', 28, semantic, 2, 0, len(body))
    return prefix + hashlib.sha256(prefix + body).digest() + body

def render(data):
    return '\n' + ''.join('    ' + ','.join(f'0x{x:02x}' for x in data[i:i+12]) + ',\n'
        for i in range(0, len(data), 12))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args(); packets = files = 0
    for path in sorted(HERE.rglob('*84*golden*.h')):
        if path.name.startswith('sdk_'): continue
        text = path.read_text(encoding='utf-8'); rows = arrays(text); replacements = {}
        for match, data in rows:
            if data.startswith(b'XRCHK\0\0\0') and struct.unpack_from('<I', data, 12)[0] == 84:
                assert frame(data[64:], 84) == data, (path.name, match[1])
                replacements[match[1]] = frame(data[64:], 85); packets += 1
        for match, data in reversed(rows):
            new = replacements.get(match[1], data)
            if match[1] == 'checked_scalar84_digest': new = replacements['checked_scalar84_golden'][32:64]
            text = text[:match.start(2)] + render(new) + text[match.end(2):]
        text = re.sub(r'\b[A-Za-z_]\w*84\w*\b', lambda m: m[0].replace('84', '85'), text)
        target = path.with_name(path.name.replace('84', '85'))
        if args.write: target.write_text(text, encoding='utf-8', newline='\n')
        else: assert target.read_text(encoding='utf-8') == text, target
        files += 1
    print(f'Independent unchanged predecessor fields: {packets} packets / {files} headers; writer unused')
    # Scalar slot zero cannot supply the owned storage or borrowed descriptor
    # required by any view operation. An empty cleanup has no owner to retire.
    models = [('empty_cleanup', [183,0,0,0,0,0,0,0,0], [])]
    for opcode in range(176,184):
        grouped = opcode in (176,177,180,183)
        result = 258 if opcode in (176,177) else (2 if opcode in (178,179) else (256 if opcode==181 else 0))
        count = 1 if opcode==183 else 3
        instruction = [opcode,result,0,count if grouped else 0,0,0,0,0,0]
        models.append(('scalar_'+str(opcode), instruction, [0]*count if grouped else []))
    models.append(('invalid_new_flag', [176,258,0,3,0,0,2,0,0], [0,0,0]))
    rows = []
    for name, instruction, operands in models:
        model = dict(linkage=0,functions=[dict(name='viewOps',parameters=[],result=0,
            blocks=[[0,3,0,0]],instructions=[[2,2,0,0,0,0,0,0,0],instruction,[33,0,0,0,0,0,0,0,0]],
            operands=operands)],declarations=None,generics=None,nodes=[[2,0,2],[3,0,256],[11,0,2],[12,0,2]],
            nominals=[],interfaces=[],defaults=[])
        body,_ = encode_body(model,2);body = body[:-8] + bytes(4) + body[-8:]
        rows.append(('view_ops85_'+name,frame(body,85)))
    header(HERE/'xir_view_ops85_golden.h','XIR_VIEW_OPS85_GOLDEN_H',rows,args.write)
    print(f'Typed view named-field packets: {len(rows)}; all eight scalar receiver rejections and empty cleanup')

if __name__ == '__main__': main()
