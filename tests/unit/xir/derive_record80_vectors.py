"""Independent complete empty, Unit, generic and mutable Record packet facts."""
from pathlib import Path
import argparse,hashlib,struct
from derive_nested_nullable_vectors import words,operation,function
from derive_construction72_vectors import header
from derive_semantic80_vectors import Cursor
ROOT=Path(__file__).resolve().parent

def blob(text):
    data=text.encode('utf-8');return words(len(data))+data

def declarations(count):
    return (words(1,0,0,0,1)+blob('record')+words(0,0,0)+
        b''.join(words(0,int(f!=0),0,0,0,0,0,0,0,0) for f in range(count))+words(0))

def record(fields,span=0):
    return words(11,span,len(fields),*[ty for name,ty in fields])+b''.join(blob(name) for name,ty in fields)

def packet(functions,types,generics=None):
    body=words(0,len(functions),1)+b''.join(functions)+declarations(len(functions))
    body+=(words(0) if generics is None else words(1)+generics)
    body+=words(len(types),0,0)+b''.join(types)+words(0,0,0)
    cursor=Cursor(body,32);cursor.module();assert cursor.at==len(body) and bytes(cursor.out)==body
    prefix=b'XRCHK\0\0\0'+words(32,80,2,0)+struct.pack('<Q',len(body))
    assert len(prefix)==32
    return prefix+hashlib.sha256(prefix+body).digest()+body

def models():
    init=function('$init',[],0,[operation(33)])
    entry=function('referenceAnswer',[],2,[operation(2,2,immediate=7),operation(33,left=0)])
    empty=packet([init,entry,function('produce',[],256,[operation(154,256),operation(33,left=0)])],[record([])])
    instructions=[operation(1,1,immediate=1),operation(2,2,immediate=7),
        operation(154,256,right=2),operation(33,left=2)]
    fields=[('a',1),('empty',0),('z',2)]
    ordinary=lambda fields:packet([init,entry,function('produce',[],256,instructions,[0,1])],[record(fields)])
    generic=packet([init,function('referenceAnswer',[],2,[operation(2,2,immediate=7),operation(33,left=0)]),
        function('identity',[256],256,[operation(33,left=0)])],[record([('value',65536)],1)],
        words(0,0,0,0,0,0,1,0,0,0,0))
    mutate=[operation(2,2,immediate=7),operation(154,256,right=1),operation(59,257,left=1),
        operation(71,256,left=2),operation(2,2,immediate=9),operation(156,left=3,right=4),
        operation(60,256,left=2),operation(155,2,left=6),operation(33,left=7)]
    # The public entry crosses an ordinary helper edge in either mixed direction.
    setter=packet([init,function('referenceAnswer',[],2,[operation(28,2,immediate=2),operation(33,left=0)]),
        function('mutate',[],2,mutate,[0])],[record([('z',2)]),words(3,0,256)])
    return [('record80_empty',empty),('record80_unit_fields',ordinary(fields)),('record80_generic',generic),
        ('record80_mutate',setter),('record80_duplicate',ordinary([('a',1),('a',0),('z',2)])),
        ('record80_unsorted',ordinary([('z',1),('empty',0),('a',2)])),
        ('record80_invalid_name',ordinary([('a',1),('empty',0),('z.z',2)]))]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    rows=models();header(ROOT/'xir_record80_golden.h','XIR_RECORD80_GOLDEN_H',rows,args.write)
    assert len(rows)==7
    print('Independent complete Record packets:',[(name,len(data),hashlib.sha256(data).hexdigest()) for name,data in rows])
if __name__=='__main__':main()
