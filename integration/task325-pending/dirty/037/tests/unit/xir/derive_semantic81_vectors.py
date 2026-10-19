"""Consume every complete semantic80 field and add explicit structural lists.

The retained 27 source packets are immutable independent inputs. Every old
constraint has an explicitly empty structural list; new nonempty contracts
are modeled separately. No product writer or historical runtime reader runs.
"""
from pathlib import Path
import argparse,hashlib,json,re,struct
from derive_semantic80_vectors import Cursor as PreviousCursor,ARRAY,octets,render
ROOT=Path(__file__).resolve().parent
WORD=lambda value:struct.pack('<I',value)
class Cursor(PreviousCursor):
    def constraints(self,count):
        for _ in range(count):
            self.word();self.applications()
            self.insertions.append(self.at);self.out+=WORD(0)

def frame(packet,with_offsets=False):
    assert packet[:8]==b'XRCHK\0\0\0'
    assert struct.unpack_from('<II',packet,8)==(32,80)
    assert struct.unpack_from('<Q',packet,24)[0]==len(packet)-64
    assert hashlib.sha256(packet[:32]+packet[64:]).digest()==packet[32:64]
    cursor=Cursor(packet[64:],32);cursor.module();assert cursor.at==len(packet)-64
    body=bytes(cursor.out)
    prefix=packet[:8]+struct.pack('<II',33,81)+packet[16:24]+struct.pack('<Q',len(body))
    assert len(prefix)==32
    result=prefix+hashlib.sha256(prefix+body).digest()+body
    return (result,tuple(64+offset for offset in cursor.insertions)) if with_offsets else result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    registry=json.loads((ROOT/'semantic81_inputs.json').read_text(encoding='utf-8'));count=0
    for row in registry:
        raw=(ROOT/row['source']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256'],row['source']
        text=raw.decode().replace('\r\n','\n');packets={};digests={};insertions={}
        for match in ARRAY.finditer(text):
            if match[1] not in row['models']:continue
            old=octets(match);new,added=frame(old,True);packets[match[1]]=new;insertions[match[1]]=added
            digests[old[32:64]]=new[32:64];count+=1
        assert set(packets)==set(row['models'])
        def replace(match):
            old=octets(match)
            return render(match[1].replace('80','81'),packets.get(match[1],digests.get(old,old)))
        output=ARRAY.sub(replace,text)
        output=re.sub(r'\b[A-Za-z_]\w*80\w*\b',lambda m:m[0].replace('80','81'),output)
        output=re.sub(r'(XR_XIR_CHECKED_SCHEMA\s*==\s*)\d+(u?)\b',r'\g<1>33\2',output)
        output=re.sub(r'(XR_XIR_CHECKED_CONTRACT\s*==\s*)\d+(u?)\b',r'\g<1>81\2',output)
        if row['target']=='root_construction81_goldens.h':
            for macro,model in (('CHECKED','root_construction72_checked'),('RECURSIVE','root_construction72_instance')):
                name='ROOT_CONSTRUCTION72_'+macro+'_OWNER'
                match=re.search(r'#define '+name+r' (\d+)u',output);assert match
                old_offset=int(match[1]);new_offset=old_offset+4*sum(p<=old_offset for p in insertions[model])
                original=next(octets(m) for m in ARRAY.finditer(text) if m[1]==model)
                assert original[old_offset:old_offset+4]==packets[model][new_offset:new_offset+4]
                output=output.replace(match[0],'#define ROOT_CONSTRUCTION81_'+macro+'_OWNER '+str(new_offset)+'u')
            output=output.replace('ROOT_CONSTRUCTION72_GOLDENS_H','ROOT_CONSTRUCTION81_GOLDENS_H')
        target=ROOT/row['target']
        if args.write:target.write_bytes(output.encode())
        else:assert target.read_text()==output,row['target']
    assert count==27,count
    print(json.dumps(dict(independent_complete_packets=count,retained_headers=len(registry),
        explicit_structural_lists_added=True,recursive_sources_consumed=True,product_writer_used=False)))
if __name__=='__main__':main()
