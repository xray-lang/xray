"""Traverse the complete retained Ordering fields into the current grammar.

Historical bytes stay immutable. No compiler writer or executable is read.
The retained declaration grammar has no lexical scope, variadic flag or
exported-constant facts; each new field is independently declared zero.
"""
from pathlib import Path
import argparse,hashlib,re,struct
from derive_construction72_vectors import header,literal
from derive_semantic84_vectors import Cursor
ROOT=Path(__file__).resolve().parent

class Ordering72Fields(Cursor):
    def declarations(self,functions):
        modules,slots,literals=self.word(),self.word(),self.word();self.take(8)
        assert modules==2 and slots==literals==0 and functions==3
        for _ in range(modules):
            self.blob();self.vector();self.word();self.insert(4)
        for _ in range(functions):self.take(36);self.insert(4)
        assert self.word()==0

def current():
    previous=literal(ROOT/'xir_ordering72_golden.h','ordering72_golden')
    assert previous[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',previous,8)==(27,72)
    assert struct.unpack_from('<Q',previous,24)[0]==len(previous)-64
    assert hashlib.sha256(previous[:32]+previous[64:]).digest()==previous[32:64]
    fields=Ordering72Fields(previous[64:],27);fields.module();assert fields.at==len(previous)-64
    prefix=previous[:8]+struct.pack('<II',32,84)+previous[16:24]+struct.pack('<Q',len(fields.out))
    packet=prefix+hashlib.sha256(prefix+fields.out).digest()+fields.out
    checked=Cursor(packet[64:],32);checked.module()
    assert checked.at==len(packet)-64 and checked.out==fields.out and not checked.changes
    text=(ROOT/'xir_ordering25_golden.h').read_text()
    macros=[]
    for name in ['ORDERING25_RECORD_OFFSET','ORDERING25_VARIANT0_OFFSET']:
        match=re.search(r'^#define\s+'+name+r'\s+(\d+)u?\s*$',text,re.M);assert match,name
        old=int(match[1]);assert old in fields.offsets;macros.append((name.replace('25','84'),fields.offsets[old]))
    return previous,packet,macros

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    previous,packet,macros=current()
    header(ROOT/'xir_ordering84_golden.h','XIR_ORDERING84_GOLDEN_H',[('ordering84_golden',packet)],args.write,macros)
    print('Independent complete Ordering fields: retained27/72=%s current32/84=%s bytes=%d offsets=%r' %
          (hashlib.sha256(previous).hexdigest(),hashlib.sha256(packet).hexdigest(),len(packet),macros))
if __name__=='__main__':main()
