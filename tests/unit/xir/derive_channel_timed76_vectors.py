"""Encode timed Channel fields and retain complete previous operation vectors."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def words(*values):return struct.pack('<'+'I'*len(values),*values)
def counted(text):
    raw=text.encode('utf-8');return words(len(raw))+raw

def timed_body(task,element,send,prelude):
    parameters=(256,)+( (element,) if send and element else () )+(2,)
    result=258 if send else 257
    op={'op':162 if send else 163,'result':result,'args':(0,len(parameters)),
        'targets':(0,0),'immediate':0,'type_args':(0,0)}
    returned=task.ins('return',args=(len(parameters),0))
    function=task.function('timedWire',parameters,result,[(0,2,0,0)],[op,returned],tuple(range(len(parameters))))
    module='stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def'
    initializer=task.function('init',(),0,[(0,1,0,0)],[task.ins('return')])
    body=words(1,2,1)+task.encode_function(initializer)+task.encode_function(function)
    # One library module, no slots/literals/dependencies/implementations; no program entry.
    body+=words(1,0,0,0xffffffff,0xffffffff)+counted(module)+words(0,0)
    body+=words(*([0]*9))+words(0,1,*([0]*7))+words(0)
    body+=words(0,3,2,0)+words(9,0,element,4,0,0,1,element,0,4,0,1,0,0)
    digest=hashlib.sha256(prelude.encode('utf-8')).digest()
    for name,native,arity,fields,variants in [
        ('Recv',7,1,[('value',65536,0)],[('Value',0,1),('Empty',1,0),('Timeout',1,0),('Closed',1,0)]),
        ('SendResult',8,0,[],[('Sent',0,0),('Full',0,0),('Timeout',0,0),('Closed',0,0)])]:
        body+=counted(module)+counted(name)+words(1,1,0,native)+digest
        body+=words(arity)+(words(0,0) if arity else b'')+words(len(fields))
        for field,type_id,flags in fields:body+=counted(field)+words(type_id,flags)
        body+=words(len(variants))
        for variant,first,count in variants:body+=counted(variant)+words(first,count)
    # Two exact construction rows; Recv has one field initializer, all absent.
    body+=words(2,0,1,0,0,0,0,0)
    return body

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--output-root',type=Path,required=True)
    parser.add_argument('--write',action='store_true');args=parser.parse_args()
    test=args.source_root/'tests/unit/xir';args.output_root.mkdir(parents=True,exist_ok=True)
    helper=load(test/'derive_construction72_vectors.py','timed76_framing')
    task=load(test/'derive_task_checked70_goldens.py','timed76_functions')
    prelude=(args.source_root/'stdlib/prelude/builtin_symbols.def').read_text(encoding='utf-8')
    assert hashlib.sha256(prelude.encode('utf-8')).hexdigest()=='7c7e1749ecae49adc12a579614991f34236c02f61497b439cb61073f9893d18c'
    previous=[];rows=[]
    for send in (True,False):
        for element,name in ((2,'i64'),(0,'unit')):
            operation='send' if send else 'recv_or'
            parameters=(256,2) if element else (256,)
            result=2 if send else element
            op={'op':159 if send else 161,'result':result,'args':(0,len(parameters)),
                'targets':(0,0),'immediate':0,'type_args':(0,0)}
            returned=task.ins('return',args=(len(parameters) if result else 0,0))
            function=task.function('blockingWire',parameters,result,[(0,2,0,0)],[op,returned],tuple(range(len(parameters))))
            body=words(0,1,0)+task.encode_function(function)+words(0,1,0,0,9,0,element,0,0,0)
            symbol='blocking_'+operation+'_'+name+'75_golden';old=helper.frame(body,28,75)
            assert old==helper.literal(test/'xir_channel_blocking75_golden.h',symbol)
            previous.append({'symbol':symbol,'bytes':len(old),'sha256':hashlib.sha256(old).hexdigest()})
            rows.append(('retained_'+operation+'_'+name+'76_golden',helper.frame(body,28,76)))
    for send in (True,False):
        for element,name in ((2,'i64'),(0,'unit')):
            body=timed_body(task,element,send,prelude)
            rows.append(('timed_'+('send' if send else 'recv')+'_'+name+'76_golden',helper.frame(body,28,76)))
    helper.header(args.output_root/'xir_channel_timed76_golden.h','XIR_CHANNEL_TIMED76_GOLDEN_H',rows,args.write)
    print(json.dumps({'schema':28,'semantic':76,'production_writer_used':False,'complete_previous75':previous,
        'rows':[{'name':n,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for n,data in rows]},indent=2))

if __name__=='__main__':main()
