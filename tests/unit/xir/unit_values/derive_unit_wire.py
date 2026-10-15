"""Reproduce the complete Unit packet from explicit fields, never product output."""
from pathlib import Path
import hashlib,re,struct

RETIRED75_SHA256 = '4c99873a49548be07eb83d2ce951b70b22bf993966e607f047aa3e1b58834b5d'

def verify_unit_packets():
    root=Path(__file__).resolve().parent
    words=lambda *values:struct.pack('<'+'I'*len(values),*values)
    def op(kind,value_type,argument=0,immediate=0):
        return words(kind,value_type,argument,0,0,0)+struct.pack('<Q',immediate)+words(0,0)
    # Program linkage; absent declarations; one named Unit-parameter function.
    body=words(0,1,0,4)+b'unit'+words(1,0,2,1,0,3,0,0,3)
    body+=op(143,0)+op(2,2,immediate=42)+op(33,0,2)
    # Operands, generics, types, nominals, interfaces, construction, defaults,
    # and provenance each retain their actual mandatory zero count.
    body+=words(0,0,0,0,0,0,0,0)
    header=b'XRCHK\0\0\0'+words(29,76,2,0)+struct.pack('<Q',len(body))
    current=header+hashlib.sha256(header+body).digest()+body
    def array(text,name):
        match=re.search(r'static const uint8_t '+name+r'\[\] = \{(.*?)\};',text,re.S)
        assert match,name
        return bytes(int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{2})',match[1]))
    source=(root.parent/'compile_owner/checked_sizing75_golden.h').read_text(encoding='utf-8')
    retired=array(source,'sizing75_golden')
    assert hashlib.sha256(retired).hexdigest()==RETIRED75_SHA256
    assert struct.unpack_from('<II',retired,8)==(29,75)
    assert hashlib.sha256(retired[:32]+retired[64:]).digest()==retired[32:64]
    target=(root/'unit_wire_golden.h').read_text(encoding='utf-8')
    assert array(target,'unit76_packet')==current
    assert array(target,'previous75_packet')==retired
    print('Independent complete Unit76 and retained valid75 packets reproduced.')

if __name__=='__main__':verify_unit_packets()
