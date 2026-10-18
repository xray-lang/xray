"""Frame the combined construction and container contract from immutable fields.

This independent walker preserves every predecessor body byte and inserts only
the three explicitly empty materialization/site/substitution counts required by
wire30 at each old wire28 construction boundary, including recursive provenance.
No product writer or runtime-produced packet is used.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

HERE = Path(__file__).resolve().parent
ARRAY = re.compile(r'static const uint8_t (\w+)\[\d*\]\s*=\s*\{(.*?)\};', re.S)

class Fields:
    def __init__(self, data, wire):
        self.data, self.wire, self.at, self.insertions = data, wire, 0, []

    def skip(self, size):
        assert 0 <= size <= len(self.data) - self.at, (self.at, size, len(self.data))
        self.at += size

    def word(self):
        value = struct.unpack_from('<I', self.data, self.at)[0]
        self.skip(4)
        return value

    def vector(self, width=4):
        self.skip(self.word() * width)

    def blob(self):
        self.vector(1)

    def application(self):
        self.word(); self.vector()

    def applications(self):
        for _ in range(self.word()): self.application()

    def constraints(self, count):
        for _ in range(count): self.word(); self.applications()

    def module(self):
        self.word(); functions, declarations = self.word(), self.word()
        for _ in range(functions):
            self.blob(); self.vector(); self.word()
            self.vector(16); self.vector(40); self.vector()
        if declarations:
            modules, slots, literals = self.word(), self.word(), self.word()
            self.skip(8)
            for _ in range(modules): self.blob(); self.vector(); self.word()
            self.skip(functions * 36 + slots * 12)
            for _ in range(literals): self.blob()
            for _ in range(self.word()):
                self.word(); self.application()
                for _ in range(self.word()): self.application(); self.skip(8)
        if self.word():
            for _ in range(functions):
                count, kinds = self.word(), self.word()
                if kinds: self.skip(count * 4)
                self.constraints(count); self.vector()
        nodes, nominals, interfaces = self.word(), self.word(), self.word()
        for _ in range(nodes):
            kind = self.word(); self.word()
            if kind == 1: self.vector(8); self.skip(8)
            elif kind == 6: self.vector()
            elif kind == 9: self.skip(8)
            elif kind == 4: self.word(); self.vector(); self.vector()
            elif kind in (2,3,5,7,8,10,11,12): self.word()
            else: raise ValueError(('unrecognized independent type shape', kind, self.at))
        for _ in range(nominals):
            self.blob(); self.blob(); self.skip(48)
            self.constraints(self.word())
            for _ in range(self.word()): self.blob(); self.skip(8)
            for _ in range(self.word()): self.blob(); self.skip(8)
        for _ in range(interfaces):
            self.blob(); self.blob(); self.word(); self.constraints(self.word())
            self.applications()
            for _ in range(self.word()):
                self.blob(); self.skip(8); self.constraints(self.word())
        for _ in range(self.word()): self.word(); self.vector()
        if self.wire == 28: self.insertions.append(self.at)
        else:
            self.vector(16); self.vector(12)
            for _ in range(self.word()): self.word(); self.vector(); self.word()
        self.vector(16)
        evidence = self.word()
        if evidence == 1:
            for _ in range(self.word()):
                self.vector(8); self.word(); self.vector(8)
                self.vector(12); self.vector(16)
        elif evidence in (2,3):
            self.module()
            for _ in range(self.word()): self.word(); self.vector(); self.vector(8)
            self.vector(24)
        else: assert evidence == 0, ('evidence', evidence, self.at)

def frame(data):
    assert data[:8] == b'XRCHK\0\0\0'
    wire, semantic, stage, flags, length = struct.unpack_from('<4IQ', data, 8)
    assert (wire, semantic) in ((28,85),(30,75)), (wire,semantic)
    assert length == len(data)-64 and hashlib.sha256(data[:32]+data[64:]).digest() == data[32:64]
    fields = Fields(data[64:], wire); fields.module()
    assert fields.at == length, (fields.at,length)
    body = data[64:]
    for at in reversed(fields.insertions): body = body[:at] + bytes(12) + body[at:]
    prefix = b'XRCHK\0\0\0' + struct.pack('<4IQ',30,86,stage,flags,len(body))
    return prefix + hashlib.sha256(prefix+body).digest() + body, fields.insertions

def render(data):
    return '\n'+''.join('    '+','.join(f'0x{x:02x}' for x in data[i:i+12])+',\n' for i in range(0,len(data),12))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write',action='store_true'); args=parser.parse_args()
    paths=list(HERE.rglob('*85*golden*.h'))
    for path in HERE.rglob('*75*golden*.h'):
        if not path.with_name(path.name.replace('75','85')).exists():
            paths.append(path)
    paths=[p for p in paths if 'retired_fixed_inputs' not in p.parts and not p.name.startswith('sdk_')]
    report=[]
    for path in sorted(paths):
        # Shared fixture families retain the base's full construction/CTFE model.
        predecessor=path.with_name(path.name.replace('85','75'))
        use_base=predecessor.exists() and ('golden' in predecessor.name) and any(
            struct.unpack_from('<I',bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',m[2])),8)[0]==30
            for m in ARRAY.finditer(predecessor.read_text('utf-8')) if len(re.findall('0x',m[2]))>=64)
        source=predecessor if use_base else path
        text=source.read_text('utf-8'); matches=list(ARRAY.finditer(text)); replacements={}
        for m in matches:
            data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',m[2]))
            if data.startswith(b'XRCHK\0\0\0') and struct.unpack_from('<I',data,12)[0] in (75,85):
                current, offsets=frame(data); replacements[m[1]]=current
                report.append(dict(source=source.relative_to(HERE).as_posix(),symbol=m[1],old_bytes=len(data),
                    bytes=len(current),insertions=offsets,sha256=hashlib.sha256(current).hexdigest()))
        for m in reversed(matches):
            data=replacements.get(m[1])
            if m[1].startswith('checked_scalar') and m[1].endswith('_digest'):
                data=next(iter(replacements.values()))[32:64]
            if data is not None: text=text[:m.start(2)]+render(data)+text[m.end(2):]
        number='75' if use_base else '85'
        text=re.sub(r'\b[A-Za-z_]\w*'+number+r'\w*\b',lambda m:m[0].replace(number,'86'),text)
        # Explicit length declarations are updated from independently framed bytes.
        for name,data in replacements.items():
            newname=name.replace(number,'86')
            text=re.sub(r'('+newname+r'\[)\d+(\])',lambda m:m[1]+str(len(data))+m[2],text)
        target=source.with_name(source.name.replace(number,'86'))
        if args.write: target.write_text(text,encoding='utf-8',newline='\n')
        else: assert target.read_text('utf-8')==text,target
    manifest=HERE/'union86_vectors.json'; expected=json.dumps(report,indent=2)+'\n'
    if args.write: manifest.write_text(expected,encoding='utf-8',newline='\n')
    else: assert manifest.read_text('utf-8')==expected
    print(f'Independent union86: {len(report)} complete packets; writer unused')

if __name__=='__main__': main()
