"""Independently traverse complete declaration and hash graphs into one identity.

Every field, recursive source, zero denominator and owned declaration is consumed.
Historical packets remain immutable inputs; the product writer is never called.
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
    def __init__(self,data,wire):self.data=data;self.at=0;self.out=bytearray();self.insertions=[];self.wire=wire
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
                self.take(40 if self.wire==31 else 36)
                if self.wire==28:self.insertions.append(self.at);self.out+=WORD(0)
            assert self.wire==31 or slots==0, "historical hash models have no slot records"
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
            elif kind in (2,3,5,7,8,10):self.word()
            elif kind==9:self.take(8)
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

def nested_current_body():
    from derive_nested_nullable_vectors import words, operation, function
    semantic=65
    def op(code, *args, **kwargs):
        assert code not in (7,8,9)
        return operation(code+8 if semantic in (64,65) and code>=10 else code,*args,**kwargs)
    functions = [
        function('$init', [], 0, [op(25)]),
        function('root', [], 2, [op(20, 257, immediate=3), op(20, 256, right=1, immediate=5),
                 op(126, 1, left=1), op(2, 2, immediate=41), op(25, left=3)], [0]),
        function('none', [], 257, [op(118, 257), op(25)]),
        function('some_none', [], 257, [op(118, 256), op(119, 257), op(25, left=1)]),
        function('some7', [], 257, [op(2, 2, immediate=7), op(119, 256),
                 op(119, 257, left=1), op(25, left=2)]),
        function('unwrap_outer', [257], 256, [op(127, 256), op(25, left=1)]),
        function('unwrap_inner', [256], 2, [op(127, 2), op(25, left=1)]),
        function('presence', [257], 1, [op(126, 1), op(25, left=1)]),
    ]
    body = (words(0, 8, 1) + b''.join(functions) + (words(1,0,0,0,1,6)+b'nested'+words(0,0,0)+b''.join(words(0,int(i!=0),0,0,0,0,0,0,0,0) for i in range(8))+words(0))
            + words(0, 2, 0, 0, 5, 0, 2, 5, 0, 256, 0, 0, 0))
    return body

def frame(packet):
    assert packet[:8]==b'XRCHK\0\0\0'
    wire,semantic=struct.unpack_from('<II',packet,8)
    assert (wire,semantic) in ((31,78),(28,75))
    assert struct.unpack_from('<Q',packet,24)[0]==len(packet)-64
    assert hashlib.sha256(packet[:32]+packet[64:]).digest()==packet[32:64]
    cursor=Cursor(packet[64:],wire);cursor.module();assert cursor.at==len(packet)-64
    header=packet[:8]+struct.pack('<II',32,79)+packet[16:24]+struct.pack('<Q',len(cursor.out))
    return header+hashlib.sha256(header+cursor.out).digest()+cursor.out

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    registry=json.loads((ROOT/'semantic79_inputs.json').read_text(encoding='utf-8'));count=0
    for row in registry:
        raw=(ROOT/row['source']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256'],row['source']
        text=raw.decode().replace('\r\n','\n');packets={};digests={}
        for match in ARRAY.finditer(text):
            old=octets(match)
            if match[1] not in row['models']:continue
            if row['source']=='xir_nested_nullable75_golden.h':
                body=nested_current_body()
                cursor=Cursor(body,31);cursor.module();assert cursor.at==len(body)
                header=b'XRCHK\0\0\0'+struct.pack('<4I',32,79,2,0)+struct.pack('<Q',len(body))
                new=header+hashlib.sha256(header+body).digest()+body
            else:new=frame(old)
            packets[match[1]]=new;digests[old[32:64]]=new[32:64];count+=1
        assert set(packets)==set(row['models'])
        def replace(match):
            old=octets(match)
            name=match[1].replace(str(row['semantic']),'79')
            return render(name,packets.get(match[1],digests.get(old,old)))
        output=ARRAY.sub(replace,text)
        output=re.sub(r'\b[A-Za-z_]\w*(?:78|75)\w*\b',lambda m:m[0].replace(str(row['semantic']),'79'),output)
        output=output.replace('SCHEMA == 31u','SCHEMA == 32u').replace('CONTRACT == 78u','CONTRACT == 79u')
        target=ROOT/row['target']
        if args.write:target.write_bytes(output.encode())
        else:assert target.read_text()==output,row['target']
    assert count==18,count
    print(json.dumps(dict(independent_complete_packets=count,retained_headers=len(registry),recursive_source_parsed=True,product_writer_used=False)))
if __name__=='__main__':main()
