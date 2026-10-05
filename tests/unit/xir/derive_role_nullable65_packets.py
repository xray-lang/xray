"""Strict ordinary role/sum wire migration from preserved complete24/63 facts."""
import hashlib,struct

def current_packet(previous, semantic=65):
    assert semantic in (64,65)
    assert previous[:8]==b'XRCHK\0\0\0' and len(previous)>=64
    assert struct.unpack_from('<4I',previous,8)==(24,63,2,0)
    assert struct.unpack_from('<Q',previous,24)[0]==len(previous)-64
    assert hashlib.sha256(previous[:32]+previous[64:]).digest()==previous[32:64]
    at=64;output=bytearray(previous);changed=set()
    def word():
        nonlocal at
        assert at+4<=len(previous)
        value=struct.unpack_from('<I',previous,at)[0];at+=4;return value
    def skip(n):
        nonlocal at
        assert 0<=n<=len(previous)-at;at+=n
    def application():
        word();skip(4*word())
    def constraints(n):
        for _ in range(n):
            word()
            for _ in range(word()):application()
    linkage,functions,declarations=word(),word(),word()
    assert linkage in (0,1) and 1<=functions<=7 and declarations in (0,1)
    for _ in range(functions):
        skip(word());skip(4*word());word();skip(16*word())
        for _ in range(word()):
            opcode=word();assert 1<=opcode<=136 and opcode not in (7,8,9)
            if opcode>=10:
                struct.pack_into('<I',output,at-4,opcode+8);changed.update(range(at-4,at))
            skip(36)
        skip(4*word())
    if declarations:
        modules,slots,literals,root,entry=word(),word(),word(),word(),word()
        assert modules==1 and slots==0
        assert (linkage==1 and root==entry==0xffffffff) or (linkage==0 and root==0 and entry<functions)
        for _ in range(modules):skip(word());skip(4*word());word()
        skip(36*functions+12*slots)
        for _ in range(literals):skip(word())
        for _ in range(word()):
            word();application()
            for _ in range(word()):application();word();word()
    if word():
        for _ in range(functions):
            parameters,kinds=word(),word()
            if kinds:skip(4*parameters)
            constraints(parameters);skip(4*word())
    types,nominals,interfaces=word(),word(),word()
    # Every admitted fixed role below explicitly proves no nominal records.
    # A nominal requires its real NativeRecord36 encoder, never this projection.
    assert types in (0,1) and nominals==0 and interfaces in (0,2)
    for _ in range(types):
        kind,span=word(),word()
        if kind==1:
            parameters=word();assert span in (1,2) and parameters==span-1
            skip(8*parameters);word();word()
        else:
            assert kind==5 and span==0 and word()==2
    for _ in range(interfaces):
        skip(word());skip(word());word();constraints(word())
        for _ in range(word()):application()
        for _ in range(word()):skip(word());word();word();constraints(word())
    skip(16*word());assert word()==0 and at==len(previous)
    assert all(output[i]==previous[i] for i in range(64,len(previous)) if i not in changed)
    struct.pack_into('<II',output,8,25,semantic)
    output[32:64]=hashlib.sha256(output[:32]+output[64:]).digest()
    return bytes(output)

def arrays(path):
    import re
    text=path.read_text(encoding='utf-8')
    return {name:bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',data))
            for name,data in re.findall(r'static const uint8_t (\w+)\[\]\s*=\s*\{(.*?)\};',text,re.S)}

def nullable_header(previous):
    lines=['/* Independent current65 sums; complete prior64 packets remain rejection evidence. */',
           '#ifndef XIR_NULLABLE65_GOLDEN_H','#define XIR_NULLABLE65_GOLDEN_H']
    assert set(previous)=={'nullable_none_golden','nullable_some_golden'}
    for role in ('none','some'):
        old=previous['nullable_'+role+'_golden'];before=current_packet(old,64);current=current_packet(old,65)
        assert current[64:]==before[64:]
        for semantic,data in ((64,before),(65,current)):
            lines+=['static const uint8_t nullable_'+role+str(semantic)+'_golden[]={']
            lines+=['    '+','.join('0x%02x'%v for v in data[at:at+12])+',' for at in range(0,len(data),12)]
            lines+=['};']
    return '\n'.join(lines+['#endif',''])
