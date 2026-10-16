"""Project explicit fresh storage after actual lexical-scope wire fields.

Only the top-level module table is traversed. Callers constructing recursive
provenance own each source-body projection and its complete framing.
"""
import struct


def receive_storage(body):
    at=0
    def word():
        nonlocal at
        assert at+4<=len(body)
        value=struct.unpack_from('<I',body,at)[0]
        at+=4
        return value
    def skip(size):
        nonlocal at
        assert 0<=size<=len(body)-at
        at+=size
    word()
    functions,declarations=word(),word()
    for _ in range(functions):
        skip(word())
        skip(word()*4)
        word()
        skip(word()*16)
        skip(word()*40)
        skip(word()*4)
    positions=[]
    if declarations:
        modules=word()
        for _ in range(4): word()
        for _ in range(modules):
            skip(word())
            skip(word()*4)
            word()  # Actual initializer ordinal.
            word()  # Actual lexical scope.
            positions.append(at)
    result=bytearray()
    previous=0
    for position in positions:
        result+=body[previous:position]+bytes(4)
        previous=position
    result+=body[previous:]
    return bytes(result),[64+p for p in positions]
