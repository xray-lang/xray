"""Reproduce complete retained75 models before framing current76 packets."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re

def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source-root',type=Path,required=True);parser.add_argument('--output-root',type=Path,required=True);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    test=args.source_root/'tests/unit/xir';out=args.output_root;out.mkdir(parents=True,exist_ok=True)
    helper=load(test/'derive_construction72_vectors.py','retained76_framing');task=load(test/'derive_task_checked70_goldens.py','retained76_task');channel=load(test/'derive_channel_checked74_goldens.py','retained76_channel');records=[];previous=[]
    def old_literal(pattern,symbol):
        for f in sorted(test.glob(pattern)):
            if re.search(r'\b'+symbol+r'\s*\[',f.read_text(encoding='utf-8')):return helper.literal(f,symbol)
        raise AssertionError(symbol)
    def rows_file(filename,models):
        rows=[]
        for symbol,body in models:
            old=helper.frame(body,28,75);assert old==old_literal(filename.replace('76','75').replace('.h','*.h'),symbol.replace('76','75'))
            previous.append(dict(name=symbol.replace('76','75'),bytes=len(old),sha256=hashlib.sha256(old).hexdigest()))
            data=helper.frame(body,28,76);rows.append((symbol,data));records.append(dict(name=symbol,bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
        helper.header(out/filename,filename.upper().replace('.','_'),rows,args.write)
    rows_file('xir_channel_types76_golden.h',[(name+'76_golden',channel.packet(element)[64:]) for name,element in [('channel_types',2),('channel_unit',0)]])
    for kind,models in [('types',[('golden',task.types_body(task.I64))]),('unit',[('golden',task.types_body(task.UNIT))]),('go',[(role,task.go_body(role)) for role in task.ROLE_NAMES])]:
        rows=[]
        for role,body70 in models:
            assert body70[-8:]==bytes(8);rows.append(('task_'+kind+'76_'+role,body70[:-8]+bytes(4)+body70[-8:]))
        rows_file('xir_task_'+kind+'76_golden.h',rows)
    words=task.words;rows=[]
    for operation,opcode in [('send',159),('recv_or',161)]:
        for name,element in [('i64',2),('unit',0)]:
            params=(256,2) if element else (256,);result=2 if operation=='send' else element
            op={'op':opcode,'result':result,'args':(0,len(params)),'targets':(0,0),'immediate':0,'type_args':(0,0)}
            f=task.function('blockingWire',params,result,[(0,2,0,0)],[op,task.ins('return',args=(len(params) if result else 0,0))],tuple(range(len(params))))
            rows.append(('blocking_'+operation+'_'+name+'76_golden',words(0,1,0)+task.encode_function(f)+words(0,1,0,0,9,0,element,0,0,0)))
    rows_file('xir_channel_blocking76_golden.h',rows)
    print(json.dumps(dict(schema=28,semantic=76,production_writer_used=False,complete_previous75=previous,current=records),indent=2))

if __name__=='__main__':main()
