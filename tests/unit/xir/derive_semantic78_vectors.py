"""Independently traverse retained complete packets and add real rest facts.

Every old field is consumed and copied. Each declaration gains one canonical
zero rest word, including recursively owned specialization sources. This code
does not import or execute the product writer or consume new product output.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct

ROOT=Path(__file__).resolve().parent
WORD=lambda value:struct.pack('<I',value)
ARRAY=re.compile(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',re.S)

def octets(match):return bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))

def render(name,data):
    return 'static const uint8_t '+name+'[] = {\n'+''.join('    '+','.join('0x%02x'%v for v in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};'

class Cursor:
    def __init__(self,data):self.data=data;self.at=0;self.out=bytearray();self.insertions=[]
    def take(self,count):
        assert 0<=count<=len(self.data)-self.at,(self.at,count,len(self.data))
        value=self.data[self.at:self.at+count];self.at+=count;self.out+=value
        return value
    def word(self):return struct.unpack('<I',self.take(4))[0]
    def vector(self,stride=4):self.take(self.word()*stride)
    def blob(self):self.take(self.word())
    def application(self):self.word();self.vector()
    def applications(self):
        for _ in range(self.word()):self.application()
    def constraints(self,count):
        for _ in range(count):self.word();self.applications()
    def module(self,depth=0):
        assert depth<=1
        self.word();functions=self.word();declarations=self.word();assert declarations in (0,1)
        for _ in range(functions):
            self.blob();self.vector();self.word();self.vector(16);self.vector(40);self.vector()
        if declarations:
            modules,slots,literals=self.word(),self.word(),self.word();self.take(8)
            for _ in range(modules):self.blob();self.vector();self.take(8)
            for _ in range(functions):
                self.take(36);self.insertions.append(self.at);self.out+=WORD(0)
            self.take(slots*20)
            for _ in range(literals):self.blob()
            for _ in range(self.word()):
                self.word();self.application()
                for _ in range(self.word()):self.application();self.take(8)
        generic=self.word();assert generic in (0,1)
        if generic:
            for _ in range(functions):
                count=self.word();kinds=self.word();assert kinds in (0,1)
                if kinds:self.take(count*4)
                self.constraints(count);self.vector()
        nodes,nominals,interfaces=self.word(),self.word(),self.word()
        for _ in range(nodes):
            kind=self.word();self.word()
            if kind==1:self.vector(8);self.take(8)
            elif kind==6:self.vector()
            elif kind in (2,3,5,7,8):self.word()
            elif kind==4:self.word();self.vector();self.vector()
            else:raise AssertionError(('type kind',kind,self.at))
        for _ in range(nominals):
            self.blob();self.blob();self.take(48);self.constraints(self.word())
            for _ in range(self.word()):self.blob();self.take(8)
            for _ in range(self.word()):self.blob();self.take(8)
        for _ in range(interfaces):
            self.blob();self.blob();self.word();self.constraints(self.word());self.applications()
            for _ in range(self.word()):self.blob();self.take(8);self.constraints(self.word())
        construction=self.word();assert construction==nominals
        for _ in range(construction):self.word();self.vector()
        self.vector(16)
        provenance=self.word();assert provenance in (0,1,2)
        if provenance==1:
            count=self.word();assert count==functions
            for _ in range(count):self.vector(8);self.word();self.vector(8);self.vector(12);self.vector(16)
        elif provenance==2:
            self.module(depth+1);count=self.word();assert count==functions
            for _ in range(count):self.word();self.vector();self.vector(8)
            self.vector(24)

def frame(packet):
    assert packet[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',packet,8)==(30,77)
    assert struct.unpack_from('<Q',packet,24)[0]==len(packet)-64
    assert hashlib.sha256(packet[:32]+packet[64:]).digest()==packet[32:64]
    cursor=Cursor(packet[64:]);cursor.module();assert cursor.at==len(packet)-64
    header=packet[:8]+struct.pack('<II',31,78)+packet[16:24]+struct.pack('<Q',len(cursor.out))
    result=header+hashlib.sha256(header+cursor.out).digest()+cursor.out
    return result,[at+64 for at in cursor.insertions]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    registry=json.loads((ROOT/'semantic78_inputs.json').read_text(encoding='utf-8'));count=0
    for row in registry:
        raw=(ROOT/row['source']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256'],row['source']
        text=raw.decode().replace('\r\n','\n');packets={};old_packets={};digests={};maps={}
        for match in ARRAY.finditer(text):
            old=octets(match)
            if old[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',old,8)!=(30,77):continue
            assert match[1] in row['models']
            new,insertions=frame(old);packets[match[1]]=new;old_packets[match[1]]=old
            digests[old[32:64]]=new[32:64];maps[match[1]]=insertions;count+=1
        assert set(packets)==set(row['models'])
        def replace(match):
            old=octets(match);return render(match[1].replace('77','78'),packets.get(match[1],digests.get(old,old)))
        output=ARRAY.sub(replace,text)
        offsets=row.get('offset_models',{});relocated=[]
        def relocate(match):
            name=match[2];models=[model for prefix,model in offsets.items() if name.startswith(prefix)]
            assert len(models)==1,(row['source'],name,models)
            model=models[0];offset=int(match[3]);shifted=offset+4*sum(at<=offset for at in maps[model])
            old=old_packets[model];new=packets[model]
            assert 64<=offset<=len(old)-4 and old[offset:offset+4]==new[shifted:shifted+4],name
            relocated.append((name,offset,shifted,old[offset:offset+4].hex()))
            return match[1]+str(shifted)+'u'
        output=re.sub(r'(#define (\w+)\s+)(\d+)u(?=\n|$)',relocate,output)
        assert bool(offsets)==bool(relocated),row['source']
        output=re.sub(r'\b[A-Za-z_]\w*77\w*\b',lambda m:m[0].replace('77','78'),output)
        output=output.replace('SCHEMA == 30u','SCHEMA == 31u').replace('CONTRACT == 77u','CONTRACT == 78u')
        if relocated:print(json.dumps(dict(target=row['target'],authenticated_offset_relocations=relocated)))
        target=ROOT/row['target']
        if args.write:target.write_bytes(output.encode())
        else:assert target.read_text()==output,row['target']
    assert count==14
    print(json.dumps(dict(independent_complete_packets=count,retained_headers=len(registry),recursive_source_parsed=True,product_writer_used=False)))
if __name__=='__main__':main()
