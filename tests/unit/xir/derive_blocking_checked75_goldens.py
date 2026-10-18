"""Encode complete blocking and retained descriptor roles without a product writer."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re,struct

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    args=parser.parse_args();test=Path(__file__).resolve().parent
    helper=load(test/'derive_construction72_vectors.py','blocking75_framing')
    task=load(test/'derive_task_checked70_goldens.py','blocking75_task')
    channel=load(test/'derive_channel_checked74_goldens.py','blocking75_channel')
    records=[]
    def emit(name,rows):
        helper.header(test/name,name.upper().replace('.','_'),rows,args.write)
        records.extend({'name':symbol,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()} for symbol,data in rows)
    def historical(kind,symbol):
        for path in sorted(test.glob('xir_task_'+kind+'74_golden*.h')):
            if re.search(r'\b'+symbol+r'\s*\[',path.read_text()):return helper.literal(path,symbol)
        raise AssertionError(symbol)
    rows=[]
    for name,element in [('channel_types',2),('channel_unit',0)]:
        old=channel.packet(element)
        assert old==helper.literal(test/'xir_channel_types74_golden.h',name+'74_golden')
        rows.append((name+'75_golden',helper.frame(old[64:],28,75)))
    emit('xir_channel_types75_golden.h',rows)
    for kind,models in [('types',[('golden',task.types_body(task.I64))]),('unit',[('golden',task.types_body(task.UNIT))]),
        ('go',[(role,task.go_body(role)) for role in task.ROLE_NAMES])]:
        rows=[]
        for role,body70 in models:
            assert body70[-8:]==bytes(8)
            body=body70[:-8]+bytes(4)+body70[-8:]
            assert helper.frame(body,28,74)==historical(kind,'task_'+kind+'74_'+role)
            rows.append(('task_'+kind+'75_'+role,helper.frame(body,28,75)))
        emit('xir_task_'+kind+'75_golden.h',rows)
    words=task.words;rows=[]
    for operation,opcode in [('send',159),('recv_or',161)]:
        for name,element in [('i64',2),('unit',0)]:
            parameters=(256,2) if element else (256,)
            result=2 if operation=='send' else element
            instruction={'op':opcode,'result':result,'args':(0,len(parameters)),'targets':(0,0),'immediate':0,'type_args':(0,0)}
            returned=task.ins('return',args=(len(parameters) if result else 0,0))
            function=task.function('blockingWire',parameters,result,[(0,2,0,0)],[instruction,returned],tuple(range(len(parameters))))
            # No source declarations/generics; Channel descriptor; no construction/default/provenance owners.
            body=words(0,1,0)+task.encode_function(function)+words(0,1,0,0,9,0,element,0,0,0)
            rows.append(('blocking_'+operation+'_'+name+'75_golden',helper.frame(body,28,75)))
    emit('xir_channel_blocking75_golden.h',rows)
    print(json.dumps({'writer_used':False,'schema':28,'semantic':75,'rows':records},indent=2))

if __name__=='__main__':main()
