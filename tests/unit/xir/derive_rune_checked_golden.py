"""Independent Rune packet framing; never invokes an Xray writer."""
import hashlib, re, struct
from pathlib import Path
def words(*v):return struct.pack('<'+'I'*len(v),*v)
def operation(op,ty,a=0,immediate=0):
    return words(op,ty,a,0,0,0)+struct.pack('<q',immediate)+words(0,0)
def packet():
    body=(words(0,1,0)+words(4)+b'rune'+words(0,2,1,0,3,0,0,3)+
        operation(128,16,immediate=0x1f600)+operation(129,2)+operation(25,0,1)+
        words(0)+words(0,0,0,0,0,0))
    prefix=b'XRCHK\0\0\0'+words(23,61,2,0)+struct.pack('<Q',len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body
if __name__=='__main__':
    text=Path(__file__).with_name('xir_rune_checked_golden.h').read_text(encoding='utf8')
    literal=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',text))
    assert literal==packet()
    assert len(literal)==264
    print('independent Rune wire23/semantic61 KAT:',hashlib.sha256(literal).hexdigest())
