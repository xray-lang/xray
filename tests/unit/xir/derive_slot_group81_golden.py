"""Construct current complete group packets from explicit field roles.

Original wire25/65 packets are verified unchanged and remain retired inputs.
No product writer or runtime reader contributes bytes to the current models.
"""
from pathlib import Path
import argparse,hashlib,json,struct
from derive_nested_nullable_vectors import words,operation,function
from derive_structural81_vectors import Cursor
from derive_construction72_vectors import header
from derive_slot_group65_golden import derive as retired_models
ROOT=Path(__file__).resolve().parent

def packet(unit):
    init_ops=([operation(145,immediate=3),operation(33)] if unit else [
        operation(2,2,immediate=1),operation(2,2,immediate=2),
        operation(143,256,right=2),operation(2,2,immediate=3),
        operation(2,2,immediate=4),operation(143,256,left=2,right=2),
        operation(145,left=4,right=2,immediate=3),operation(33)])
    init=function('init',[],0,init_ops,() if unit else [0,1,3,4,2,5])
    entry=function('main',[],2,[operation(2,2,immediate=41),operation(33)])
    body=words(0,2,1)+init+entry
    # One actual lexical module, three private immutable slots, entry one.
    body+=words(1,3,0,0,1)+words(4)+b'root'+words(0,0,0)
    body+=words(0,0,0,0,0,0,0,0,0,0)
    body+=words(0,1,0,0,0,0,0,0,0,0)
    for ty in ([0,0,0] if unit else [256,0,256]):
        body+=words(0,ty,0,0,0)
    body+=words(0,0)  # No implementations or generic records.
    body+=words(1,0,0,6,0,2,2,2)  # Owned Tuple2 of i64, span zero.
    body+=words(0,0,0)  # Dense empty construction, defaults and provenance.
    # The structural cursor extends the unchanged schema32 declaration encoding.
    cursor=Cursor(body,32);cursor.module()
    assert cursor.at==len(body) and bytes(cursor.out)==body
    prefix=b'XRCHK\0\0\0'+words(33,81,2,0)+struct.pack('<Q',len(body))
    result=prefix+hashlib.sha256(prefix+body).digest()+body
    start=64+12+4+len('init')+4+4+4+16+4
    group=start if unit else start+6*40
    payload=start+len(init_ops)*40+4+4*4 if not unit else 0
    assert struct.unpack_from('<I',result,group)[0]==145
    if not unit:assert struct.unpack_from('<I',result,payload)[0]==2
    return result,group,payload,start

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    historical,old_rows,old_packets=retired_models()
    assert (ROOT/'slot_group65_golden.h').read_text(encoding='utf-8')==historical
    rows=[];macros=[]
    for name,unit in [('slot_group_mixed81_golden',False),('slot_group_unit81_golden',True)]:
        data,group,payload,start=packet(unit);rows.append((name,data))
        label='SLOT_GROUP81_UNIT' if unit else 'SLOT_GROUP81_MIXED'
        macros.extend([(label+'_GROUP_OFFSET',group),(label+'_INSTRUCTION_OFFSET',start)])
        if not unit:macros.append((label+'_PAYLOAD_OFFSET',payload))
    header(ROOT/'slot_group81_golden.h','SLOT_GROUP81_GOLDEN_H',rows,args.write,macros)
    print(json.dumps(dict(current_complete_packets=[
        dict(name=n,length=len(b),sha256=hashlib.sha256(b).hexdigest()) for n,b in rows],
        original_complete_65_packets_unchanged=old_rows,product_writer_used=False)))
if __name__=='__main__':main()
