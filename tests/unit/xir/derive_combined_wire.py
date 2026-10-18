"""Compose independent literal tables with lexical scope and module storage.

The frozen input packets declare all bytes from both parent field models.
Only the explicit source-module table is extended. No executable, codec,
compiler-produced packet or output from a test is consulted.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from checked_scope_wire import receive_scopes, receive_offset
from checked_storage_wire import receive_storage

ROOT=Path(__file__).resolve().parent
ARRAY=re.compile(r'static const uint8_t (\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',re.S)

def frame(body):
    header=b'XRCHK\0\0\0'+struct.pack('<IIIIQ',31,76,2,0,len(body))
    return header+hashlib.sha256(header+body).digest()+body

def literal(name,data):
    lines=['static const uint8_t '+name+'[] = {']
    lines+=['    '+', '.join('0x%02x'%b for b in data[i:i+12])+',' for i in range(0,len(data),12)]
    return '\n'.join(lines)+'\n};'

def derive():
    inputs=json.loads((ROOT/'combined_wire_inputs.json').read_text(encoding='utf-8'))
    headers={}
    for row in inputs['headers']:
        assert hashlib.sha256(row['text'].encode()).hexdigest()==row['sha256']
        headers[row['commit'],row['path']]=row['text']
    for row in inputs['records']:
        raw=bytes.fromhex(row['hex'])
        assert hashlib.sha256(raw).hexdigest()==row['sha256']
        assert struct.unpack_from('<Q',raw,24)[0]==len(raw)-64
        assert hashlib.sha256(raw[:32]+raw[64:]).digest()==raw[32:64]
    source_commit='650de5cd340206dcb0f71a7eaf70a758abcf02f6'
    storage_commit='f0e9360091d7c4dcb66a1fa1e32112498e6a9b78'
    outputs={}
    families=['xir_assert_panics','xir_checked_range','xir_checked_scalar',
              'xir_error_marker','xir_generic_method','xir_rune',
              'comptime_binding','materialization']
    names=[(name+'75_golden.h',name+'76_golden.h') for name in families]
    names += [('compile_owner/checked_sizing75_golden.h','compile_owner/checked_sizing76_golden.h'),
              ('xir_defaults75_golden_bytes.h','xir_defaults76_golden_bytes.h'),
              ('xir_library_string75_goldens.h','xir_library_string76_goldens.h')]
    for old,new in names:
        text=headers[source_commit,'tests/unit/xir/'+old]
        positions=[];digest=None
        def array(match):
            nonlocal positions,digest
            name=match[1].replace('75','76')
            raw=bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            if raw[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',raw,8)==(30,75):
                scoped,scope_positions=receive_scopes(raw[64:])
                body,storage_positions=receive_storage(scoped)
                positions=scope_positions
                assert storage_positions==[p+4*(i+1) for i,p in enumerate(scope_positions)]
                raw=frame(body);digest=raw[32:64]
            elif match[1].endswith('75_digest'):
                assert digest is not None;raw=digest
            return literal(name,raw)
        text=ARRAY.sub(array,text)
        def macro(match):
            name,value=match[1],int(match[2])
            if any(token in name for token in ['VECTOR_','_OWN','CALLABLE_FLAGS','KINDS_PRESENT','_MARKER','BINDING75_']):
                value=receive_offset(value,positions)
                shifted=[p+4*(i+1) for i,p in enumerate(positions)]
                value=receive_offset(value,shifted)
            return '#define '+name.replace('75','76')+' '+str(value)+match[3]
        text=re.sub(r'#define (\w*75\w*) (\d+)(u?)\b',macro,text)
        text=text.replace('75_GOLDEN_H','76_GOLDEN_H').replace('75_GOLDENS_H','76_GOLDENS_H')
        text=text.replace('75_GOLDEN_BYTES_H','76_GOLDEN_BYTES_H').replace(old,new)
        outputs[new]=text
    for name in ['slot_group76_golden.h','compile_owner/birth_storage76_golden.h']:
        text=headers[storage_commit,'tests/unit/xir/'+name]
        def storage_array(match):
            raw=bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
            if raw[:8]==b'XRCHK\0\0\0' and struct.unpack_from('<II',raw,8)==(30,76):
                assert raw[-8:]==bytes(8)
                raw=frame(raw[64:-8]+bytes(12)+raw[-8:])
            return literal(match[1],raw)
        text=ARRAY.sub(storage_array,text).replace('SCHEMA==30u','SCHEMA==31u').replace('wire30','wire31')
        outputs[name]=text
    text=headers[storage_commit,'tests/unit/xir/root_construction72_goldens.h']
    raw_arrays={m[1]:bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',m[2])) for m in ARRAY.finditer(text)}
    checked=raw_arrays['root_construction72_checked'][64:]
    owner=int(re.search(r'#define ROOT_CONSTRUCTION72_CHECKED_OWNER (\d+)u',text)[1])-64
    # One nominal with two actual default helpers, followed by zero defaults.
    assert checked[owner:owner+24]==struct.pack('<6I',1,3,2,4,5,0)
    checked_new=checked[:owner+20]+bytes(12)+checked[owner+20:]
    instance=raw_arrays['root_construction72_instance'][64:]
    start=instance.index(checked)
    assert instance.find(checked,start+1)<0 and instance[start-8:start]==struct.pack('<II',0,2)
    instance_new=instance[:start-8]+bytes(12)+instance[start-8:start]+checked_new+instance[start+len(checked):]
    def root_array(match):
        name=match[1]
        if name=='root_construction72_checked':return literal(name,frame(checked_new))
        if name=='root_construction72_instance':return literal(name,frame(instance_new))
        return match[0]
    text=ARRAY.sub(root_array,text)
    text=re.sub(r'(#define ROOT_CONSTRUCTION72_RECURSIVE_OWNER )(\d+)u',lambda m:m[1]+str(int(m[2])+12)+'u',text)
    outputs['root_construction72_goldens.h']=text
    history=['/* Exact complete parent packets, rejected by the combined wire reader. */',
             '#ifndef COMBINED_PREVIOUS_WIRE_H','#define COMBINED_PREVIOUS_WIRE_H']
    chunk=[];chunk_lines=0;chunks=0
    def flush():
        nonlocal chunk,chunk_lines,chunks
        name='combined_wire_previous_%02d.h'%chunks
        outputs[name]='/* Preserved independent parent bytes. */\n'+'\n'.join(chunk)+'\n'
        history.append('#include "'+name+'"');chunks+=1;chunk=[];chunk_lines=0
    for i,row in enumerate(inputs['records']):
        text=literal('combined_previous_'+str(i),bytes.fromhex(row['hex']))
        lines=len(text.splitlines())
        if chunk_lines+lines>600:flush()
        chunk.append(text);chunk_lines+=lines
    if chunk:flush()
    history.append('static const struct { const uint8_t *bytes; size_t length; } combined_previous_wire[] = {')
    history+=['    {combined_previous_%d,sizeof(combined_previous_%d)},'%(i,i) for i in range(len(inputs['records']))]
    history+=['};','#endif // COMBINED_PREVIOUS_WIRE_H']
    outputs['combined_previous_wire.h']='\n'.join(history)+'\n'
    return outputs

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true')
    parser.add_argument('--source-root',type=Path);parser.add_argument('--output-root',type=Path)
    parser.add_argument('--check',type=Path)
    args=parser.parse_args();outputs=derive()
    for name,text in outputs.items():
        path=ROOT/name
        if args.write:path.write_text(text,encoding='utf-8',newline='\n')
        else:assert path.read_text(encoding='utf-8')==text,name
    print(json.dumps(dict(wire=31,semantic=76,headers=len(outputs),product_codec_used=False)))

if __name__=='__main__':main()
