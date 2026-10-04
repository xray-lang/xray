"""Migrate ordinary wire21 literal facts to wire22 with canonical TYPE roles.

This independent cursor never executes XIR or reads a verifier output. Every old
frame is first validated against its stored length and digest, then unchanged
fields are copied and the sole new per-function role presence word is inserted.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct


class Frame:
    def __init__(self,data):
        self.data=data;self.at=0;self.output=bytearray();self.insertions=[]

    def take(self,count):
        assert count>=0 and count<=len(self.data)-self.at
        result=self.data[self.at:self.at+count];self.at+=count;self.output+=result;return result

    def word(self):return struct.unpack('<I',self.take(4))[0]
    def blob(self):self.take(self.word())
    def app(self):self.word();self.take(self.word()*4)
    def apps(self):
        for _ in range(self.word()):self.app()
    def constraints(self,count):
        for _ in range(count):self.word();self.apps()

    def module(self):
        kind=self.word();assert kind in (0,1)
        count=self.word();decl=self.word();assert decl in (0,1)
        for _ in range(count):
            self.blob();self.take(self.word()*4);self.word();self.take(self.word()*16)
            self.take(self.word()*40);self.take(self.word()*4)
        if decl:
            modules=self.word();slots=self.word();literals=self.word();self.take(8)
            for _ in range(modules):self.blob();self.take(self.word()*4);self.word()
            self.take(count*28+slots*12)
            for _ in range(literals):self.blob()
            for _ in range(self.word()):
                self.word();self.app()
                for _ in range(self.word()):self.app();self.take(8)
        present=self.word();assert present in (0,1)
        if present:
            for _ in range(count):
                parameters=self.word();self.insertions.append(self.at);self.output+=struct.pack('<I',0)
                self.constraints(parameters);self.take(self.word()*4)
        nodes=self.word();nominals=self.word();interfaces=self.word()
        for _ in range(nodes):
            node=self.word();self.word()
            if node==1:self.take(self.word()*8);self.take(8)
            elif node in (2,3):self.word()
            elif node==4:self.word();self.take(self.word()*4);self.take(self.word()*4)
            else:raise AssertionError(f'unsupported node{node}')
        for _ in range(nominals):
            self.blob();self.blob();self.take(12);self.constraints(self.word())
            for _ in range(self.word()):self.blob();self.take(8)
            for _ in range(self.word()):self.blob();self.take(8)
        for _ in range(interfaces):
            self.blob();self.blob();self.word();self.constraints(self.word());self.apps()
            for _ in range(self.word()):self.blob();self.take(8);self.constraints(self.word())
        self.take(self.word()*16)
        provenance=self.word();assert provenance in (0,1)
        if provenance:
            self.module()
            for _ in range(count):self.word();self.take(self.word()*4)


def migrate(old):
    assert old[:8]==b'XRCHK\0\0\0' and len(old)>=64
    assert struct.unpack_from('<II',old,8)==(21,55)
    assert struct.unpack_from('<Q',old,24)[0]==len(old)-64
    assert hashlib.sha256(old[:32]+old[64:]).digest()==old[32:64]
    frame=Frame(old[64:]);frame.module();assert frame.at==len(frame.data)
    body=bytes(frame.output);head=bytearray(old[:32]);struct.pack_into('<II',head,8,22,56);struct.pack_into('<Q',head,24,len(body))
    return bytes(head)+hashlib.sha256(head+body).digest()+body,frame.insertions


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    directory=Path(__file__).parent;manifest_path=directory/'binder56_ordinary_vectors.json'
    if not args.write:
        records=json.loads(manifest_path.read_text(encoding='utf-8'))
        for record in records:
            new,positions=migrate(bytes.fromhex(record['old_hex']))
            assert new.hex()==record['current_hex'] and positions==record['insertions']
            text=(directory/record['path']).read_text(encoding='utf-8')
            match=re.search(r'\b'+record['name']+r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S)
            literal=bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',match[1]))
            from derive_equal57_migration import migrate as equal_frame, nullable_frame
            from derive_test_roles_vectors import upgrade
            from derive_semantic61_migration import semantic61_packet
            assert literal==semantic61_packet(upgrade(nullable_frame(equal_frame(new))))
        print(json.dumps({'old55_digest_reproductions':len(records),'old56_independent_framing':len(records),
                          'historical58_independent_framing':len(records),'current61_independent_literals':len(records)}));return
    assert not manifest_path.exists();records=[]
    for path in sorted(directory.glob('*')):
        if path.suffix not in ('.c','.h'):continue
        text=path.read_text(encoding='utf-8');edits=[]
        for match in re.finditer(r'(?:static\s+)?const\s+uint8_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S):
            old=bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',match[2]))
            if len(old)<64 or old[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',old,8)!=(21,55):continue
            new,positions=migrate(old)
            records.append({'path':path.name,'name':match[1],'old_hex':old.hex(),'current_hex':new.hex(),
                            'insertions':positions,'old_sha256':hashlib.sha256(old).hexdigest(),'current_sha256':hashlib.sha256(new).hexdigest()})
            body='\n'+'\n'.join('    '+','.join(f'0x{v:02x}' for v in new[i:i+12])+',' for i in range(0,len(new),12))+'\n'
            edits.append((match.start(2),match.end(2),body))
        for first,last,new in reversed(edits):text=text[:first]+new+text[last:]
        if edits:path.write_text(text,encoding='utf-8')
    manifest_path.write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
    print(json.dumps([{'path':r['path'],'name':r['name'],'insertions':len(r['insertions'])} for r in records],indent=2))


if __name__=='__main__':main()
