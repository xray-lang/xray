"""Frame the complete current Core from independent declaration and CFG facts."""
from pathlib import Path
import argparse,hashlib,json,re
from derive_assert_panics_vector import words,op,function,packet,vector as old_vector
from derive_semantic61_migration import semantic61_packet
from derive_tuple62_migration import tuple62_packet
from derive_tuple63_migration import tuple63_packet
from derive_assert_atomic64_packet import current64_packet

def vector(semantic=59):
    # Fixed intrinsic inventory: init, condition, panic action, ordinary Equal.
    blocks=[(0,1,0,0),(1,1,4,0),(2,4,0,0),(6,2,0,0),(8,2,0,0)]
    panic_ops=[op(23,normal=1),op(92,65536,normal=2,error=3),op(116,immediate=1),
        op(1,1),op(115,left=5,right=1),op(25),op(94,14,immediate=1),op(22,left=8),op(97),op(25)]
    facts=[('$init',[],0,[(0,1,0,0)],[op(25)]),
        ('assert',[1,3],0,[(0,2,0,0)],[op(115,right=1),op(25)]),
        ('assertPanics',[256,3],0,blocks,panic_ops),
        ('assertEqual',[65536,65536,3],0,[(0,3,0,0)],[op(117,1,right=1),op(115,left=3,right=2),op(25)]),
        ('$argument_default',[],3,[(0,2,0,0)],[op(3,3),op(25)]),
        ('$argument_default',[],3,[(0,2,0,0)],[op(3,3,immediate=1),op(25)]),
        ('$argument_default',[],3,[(0,2,0,0)],[op(3,3,immediate=2),op(25)])]
    body=words(1,7,1);offsets={}
    for index,(name,params,result,cfg,instructions) in enumerate(facts):
        offsets[f'function{index}']=64+len(body)
        offsets[f'op{index}']=64+len(body)+4+len(name)+4+4*len(params)+4+4+16*len(cfg)+4
        body+=function(name,params,result,cfg,instructions)
    identity=b'memory-module-v1:id=23:xray-core-assertions-v1'
    body+=words(1,0,3,0xffffffff,0xffffffff)+words(len(identity))+identity+words(0,0)
    for index,exported in enumerate((0,1,1,1,0,0,0)):
        offsets[f'identity{index}']=64+len(body)
        body+=words(0,exported,0,0,0,0,0)
        if semantic>=59: body+=words(0,0)
    body+=words(0,0,0,0) # Three empty literals and no implementations.
    body+=words(1)
    for index in range(7):
        offsets[f'generic{index}']=64+len(body)
        body+=words(1,1,1,0,0,0) if index in (2,5) else words(1,0,4,0,0) if index in (3,6) else words(0,0,0)
    body+=words(1,0,0,1,1,0,65536,0)
    offsets['defaults']=64+len(body)
    body+=words(3,0,1,1,4,0,2,1,5,0,3,2,6)+words(0)
    return packet(body,23 if semantic>=59 else 22,semantic),offsets

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    directory=Path(__file__).parent;old,old_offsets=old_vector()
    historical=json.loads((directory/'assert_panics_packet_vectors.json').read_text(encoding='utf-8'))
    assert old.hex()==historical['core_packet_hex'] and old_offsets==historical['offsets']
    current,offsets=vector()
    previous,previous_offsets=vector(57)
    previous58,previous58_offsets=vector(58)
    assert previous_offsets==previous58_offsets
    data={'wire':23,'semantic':59,'historical56_reproduced':True,
        'previous58_core_packet_hex':previous58.hex(),
        'previous58_sha256':hashlib.sha256(previous58).hexdigest(),
        'previous57_core_packet_hex':previous.hex(),
        'previous57_sha256':hashlib.sha256(previous).hexdigest(),
        'old56_sha256':hashlib.sha256(old).hexdigest(),'core_packet_hex':current.hex(),
        'core_body_sha256':hashlib.sha256(current[64:]).hexdigest(),
        'core_packet_sha256':hashlib.sha256(current).hexdigest(),'offsets':offsets}
    executable=tuple63_packet(tuple62_packet(semantic61_packet(current)))
    previous_literal=bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',
        (directory/'xir_assert_equal_golden.h').read_text(encoding='utf-8')))
    assert previous_literal==executable
    executable=current64_packet(executable)
    header='/* Complete independently framed wire25 semantic64 Core declaration. */\nstatic const uint8_t assert_equal64_golden[]={\n'
    header+='\n'.join('    '+','.join(f'0x{byte:02x}' for byte in executable[at:at+12])+',' for at in range(0,len(executable),12))+'\n};\n'
    if args.write:
        (directory/'xir_assert_equal64_golden.h').write_text(header,encoding='utf-8')
        assert json.loads((directory/'assert_equal_packet_vectors.json').read_text(encoding='utf-8'))==data
    else:
        assert json.loads((directory/'assert_equal_packet_vectors.json').read_text(encoding='utf-8'))==data
        text=(directory/'xir_assert_equal64_golden.h').read_text(encoding='utf-8')
        assert bytes(int(v,16) for v in re.findall(r'0x[0-9a-fA-F]{2}',text))==executable
    print(json.dumps({'bytes':len(current),'historical59_sha256':data['core_packet_sha256'],'current64_sha256':hashlib.sha256(executable).hexdigest(),'old56_reproduced':True,'offsets':offsets},indent=2))

if __name__=='__main__':main()
