"""Complete independent ordinary-role and sum models for construction packets.

The legacy full models are reproduced before their current peers are written.
Compiler writer output is never an input to these expectations.
"""
from pathlib import Path
import hashlib,json,struct
from derive_library2772_vectors import encode_body,MODEL_SHA
from derive_callable71_checked_vectors import MODELS
from derive_assert_panics71_vector import core_body,OPCODES

HERE=Path(__file__).resolve().parent

def words(*values):return struct.pack('<'+'I'*len(values),*values)

def frame(body,wire,semantic):
    prefix=b'XRCHK\0\0\0'+words(wire,semantic,2,0)+struct.pack('<Q',len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body

def instruction(opcode,result=0,left=0,right=0,immediate=0):
    return [opcode,result,left,right,0,0,immediate,0,0]

def function(name,parameters,result,instructions):
    return dict(name=name,parameters=parameters,result=result,
                blocks=[[0,len(instructions),0,0]],instructions=instructions,operands=[])

def nullable_model(some):
    code=([instruction(2,2,immediate=7),instruction(127,256)] if some else
          [instruction(126,256)])+[instruction(33,left=int(some))]
    return dict(linkage=0,functions=[function('n',[],256,code)],declarations=None,
                generics=None,nodes=[[5,0,2]],nominals=[],interfaces=[],defaults=[])

def condition_model():
    functions=[function('$init',[],0,[instruction(33)]),
               function('assert',[1,3],0,[instruction(123,right=1),instruction(33)]),
               function('$argument_default',[],3,[instruction(3,3),instruction(33)])]
    declarations=dict(modules=[['memory-module-v1:id=23:xray-core-assertions-v1',[],0]],
                      identities=[[0,e,0,0,0,0,0,0,0] for e in (0,1,0)],
                      slots=[],literals=[''],root=0xffffffff,entry=0xffffffff)
    return dict(linkage=1,functions=functions,declarations=declarations,generics=None,
                nodes=[],nominals=[],interfaces=[],defaults=[[0,1,1,2]])

def packet(symbol,wire=27,semantic=72):
    assert (wire,semantic) in ((25,65),(26,71),(27,72))
    current=wire==27
    if symbol in ('assert_equal_golden','assert_panics_golden'):
        body,offsets=core_body(OPCODES,root_upper=0 if semantic==65 else 8,
                               template=semantic!=65)
        if current:
            offset=offsets['defaults']-64
            body=body[:offset]+words(0)+body[offset:]
    else:
        if symbol.startswith('nullable_'):
            assert symbol in ('nullable_none_golden','nullable_some_golden')
            model=nullable_model(symbol=='nullable_some_golden')
        elif symbol=='assert_condition_golden':model=condition_model()
        elif symbol=='generic_method_golden':model=MODELS['14']
        else:
            model_bytes=(HERE/'library2671_models.json').read_bytes()
            assert hashlib.sha256(model_bytes).hexdigest()==MODEL_SHA
            key=symbol.replace('library_string_','library_string71_').replace('defaults_','defaults71_',1)
            model=json.loads(model_bytes)[key]
        body,_=encode_body(model,0 if semantic==65 else 8,current)
    return frame(body,wire,semantic)
