"""Encode the complete Array/place fixture from explicit independent fields."""
from pathlib import Path
import argparse
import hashlib
import json
import sys

sys.dont_write_bytecode=True
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent/'composed_wire'))
from model import encode,decode
from derive import array

def op(name,type=0,args=(0,0),immediate=0):
    return dict(op=name,type=type,arg0=args[0],arg1=args[1],target0=0,target1=0,
                immediate=immediate,type_arg0=0,type_arg1=0)

def function(name,parameters,result,instructions,operands=()):
    return dict(name=name.encode('ascii'),parameters=parameters,result=result,
                blocks=[[0,len(instructions),0,0]],instructions=instructions,operands=list(operands))

def facts():
    init=[op('ARRAY_NEW',256),op('CELL_NEW',257),op('SLOT_INIT',args=(1,0)),op('RETURN')]
    entry=[op('CONST_INT',2,immediate=42),op('CONST_INT',2,immediate=43),
           op('ARRAY_NEW',256,(0,2)),op('LOCAL_NEW',256,(2,0)),op('CELL_NEW',257,(2,0)),
           op('CELL_PLACE',256,(4,0)),op('SLOT_LOAD',257),op('CELL_PLACE',256,(6,0)),
           op('CONST_INT',2),op('ARRAY_GET',2,(5,8)),op('ARRAY_SET',args=(2,3)),
           op('ARRAY_PUSH',args=(5,9)),op('ARRAY_GET',2,(3,8)),op('ARRAY_LEN',2,(7,0)),
           op('RETURN',args=(13,0))]
    worker=[op('ARRAY_LEN',2),op('RETURN',args=(1,0))]
    return dict(linkage=0,functions=[function('init',[],0,init),
                function('entry',[],2,entry,[0,1,3,8,1]),function('read',[256],2,worker)],
        declarations=dict(root=0,entry=1,modules=[dict(name=b'app',dependencies=[],initializer=0,lexical_scope=0)],
            identities=[dict(module=0,exported=0,nominal_owner=0,member_access=0,cleanup_owner=0,promises=0,
                             method_kind=0,test_role=0,test_timeout_seconds=0,variadic=0,derivation=0) for _ in range(3)],
            slots=[dict(module=0,type=257,mutable=1,exported=0,name=0)],literals=[],implementations=[],policies=[],grants=[]),
        generics=None,nodes=[dict(kind='ARRAY',span=0,element=2),dict(kind='CELL',span=0,element=256),
                             dict(kind='CALLABLE',span=0,parameters=[],result=2,flags=8)],
        nominals=[],interfaces=[],construction=[],materialization=[],comptime_sites=[],comptime_bindings=[],
        defaults=[],provenance=dict(kind=0))

def build():
    original=facts()
    packet,offsets=encode(original)
    assert encode(decode(packet,'current')[0])[0]==packet
    prior,prior_offsets=encode(original,'P10')
    assert len(prior)==1328 and len(packet)==1360
    names={'ARRAY_PACKET_ENTRY':'module.functions[1].instructions[0].op',
           'ARRAY_PACKET_OPERANDS':'module.functions[1].operands[0]',
           'ARRAY_PACKET_SLOT':'module.declarations.slots[0].module'}
    text='/* Complete independent Array/place fields; no product writer is an oracle. */\n'
    text+=array('array_metadata_current',packet)
    for name,key in names.items():text+='#define '+name+' '+str(offsets[key][0])+'u\n'
    report=dict(bytes=len(packet),sha256=hashlib.sha256(packet).hexdigest(),
                prior_profile_bytes=len(prior),prior_field_offsets=prior_offsets,field_offsets=offsets,
                product_writer_used=False)
    return text,json.dumps(report,indent=2)+'\n'

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    text,report=build()
    output=args.output or HERE
    if args.output:
        output.mkdir(parents=True,exist_ok=True)
        (output/'current.inc.c').write_text(text,encoding='utf-8',newline='\n')
        (output/'fields.json').write_text(report,encoding='utf-8',newline='\n')
    else:
        assert (output/'current.inc.c').read_text(encoding='utf-8')==text
        assert (output/'fields.json').read_text(encoding='utf-8')==report
    print(report)
