"""Reuse fixed family roles to derive the appended-opcode current contract."""
from pathlib import Path
import argparse,hashlib,json,re,struct
from compile_owner.derive_checked_sizing64_golden import fixed_program
from derive_rune_checked_golden import packet as rune_packet
from derive_library_atomic64_vectors import arrays,current_packet
from derive_assert_atomic64_packet import current64_packet
from derive_assert_condition_vector import current as condition63,words,op,function
from derive_assert_equal_vector import vector as equal59
from derive_semantic61_migration import semantic61_packet
from derive_tuple62_migration import tuple62_packet
from derive_tuple63_migration import tuple63_packet
from derive_equal57_migration import atomic64_scalar
from derive_nested_nullable_vectors import nested_packet,single_control

DIRECTORY=Path(__file__).resolve().parent
ROOT=DIRECTORY.parents[2]

def frame(payload,semantic):
    prefix=b'XRCHK\0\0\0'+words(25,semantic,2,0)+struct.pack('<Q',len(payload))
    return prefix+hashlib.sha256(prefix+payload).digest()+payload

def counted(raw):return words(len(raw))+raw

def atomic_types(semantic):
    # Exact existing three-parameter signature: Atomic<i64>, Atomic<bool>,
    # Atomic<f64>. No nominal, callable, default or provenance metadata.
    body=words(0,1,0)+function('types',[256,257,258],0,[op(33)])
    body+=words(0,3,0,0)+words(7,0,2,7,0,1,7,0,13)+words(0,0)
    return frame(body,semantic)

def ordering(semantic):
    # Named governed enum fixture: exact root dependency, native record and
    # five fieldless variants. This is not a general nominal wire encoder.
    module=b'stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def'
    body=words(0,3,1)+function('root_init',[],0,[op(33)])
    body+=function('ordering_init',[],0,[op(33)])
    body+=function('main',[],2,[op(2,2,immediate=7),op(33)])
    body+=words(2,0,0,0,2)+counted(b'root')+words(1,1,0)+counted(module)+words(0,1)
    body+=words(0,0,0,0,0,0,0,0,0)+words(1,0,0,0,0,0,0,0,0)+words(0,1,0,0,0,0,0,0,0)
    body+=words(0,0,1,1,0,4,0,0,0,0)
    body+=counted(module)+counted(b'Ordering')+words(1,1,0,4)
    body+=bytes.fromhex('7c7e1749ecae49adc12a579614991f34236c02f61497b439cb61073f9893d18c')
    body+=words(0,0,5)
    for name in (b'Relaxed',b'Acquire',b'Release',b'AcquireRelease',b'SeqCst'):
        body+=counted(name)+words(0,0)
    body+=words(0,0)
    return frame(body,semantic)

def fixed_packet(record,semantic):
    name=record['name']
    if name.startswith('sizing65_'):
        names={'sizing65_golden':b'main','sizing65_embedded_golden':b'm\0in','sizing65_empty_golden':b''}
        return fixed_program(names[name],25,semantic,33)
    if name=='rune65_golden':return rune_packet(25,semantic,136,137,33)
    if name=='assert_condition65_golden':return current64_packet(condition63,semantic)
    if name in ('assert_equal65_golden','assert_panics65_golden'):
        old,_=equal59();return current64_packet(tuple63_packet(tuple62_packet(semantic61_packet(old))),semantic)
    if name=='atomic_types65_golden':return atomic_types(semantic)
    if name=='checked_scalar65_golden':
        from derive_panic_carrier_vector import packet
        from derive_test_roles_vectors import upgrade
        previous=tuple63_packet(tuple62_packet(semantic61_packet(upgrade(packet(58,22)))))
        return atomic64_scalar(previous,semantic)
    if name.startswith('defaults65_golden_'):
        return current_packet(arrays(DIRECTORY/'xir_defaults_golden_bytes.h')[name.replace('65','')],semantic)
    if name=='defaults65_invoke_golden':
        return current_packet(arrays(DIRECTORY/'xir_defaults_invoke_golden.h')['defaults_invoke_golden'],semantic)
    if name=='generic_method65_golden':
        return current_packet(arrays(DIRECTORY/'xir_generic_method_golden.h')['generic_method_golden'],semantic)
    if name.startswith('library_string65_'):
        return current_packet(arrays(DIRECTORY/'xir_library_string_goldens.h')[name.replace('string65_','string_')],semantic)
    if name=='ordering65_golden':return ordering(semantic)
    if name=='nested_nullable65_golden':return nested_packet(semantic)
    if name=='nested_single65_golden':return single_control(semantic)
    raise AssertionError('unnamed role: '+name)

def array(name,raw):
    lines=['static const uint8_t '+name+'[] = {']
    lines+=['    '+','.join('0x%02x'%x for x in raw[at:at+12])+',' for at in range(0,len(raw),12)]
    return '\n'.join(lines+['};',''])

def old_bytes(record):
    previous=record['previous'];text=(ROOT/previous['path']).read_text(encoding='utf-8')
    match=re.search(r'const\s+uint8_t\s+'+previous['name']+r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};',text,re.S)
    assert match
    old=bytes(int(x,16) for x in re.findall(r'0x[0-9a-fA-F]{2}',match[1]))
    assert len(old)==previous['bytes'] and hashlib.sha256(old).hexdigest()==previous['sha256']
    assert struct.unpack_from('<4I',old,8)==(25,64,2,0)
    assert struct.unpack_from('<Q',old,24)[0]==len(old)-64
    assert hashlib.sha256(old[:32]+old[64:]).digest()==old[32:64]
    return old

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    model=json.loads((DIRECTORY/'semantic65_packet_manifest.json').read_text(encoding='utf-8'))
    assert model['contract']=={'wire':25,'previous_semantic':64,'current_semantic':65,'stage':2,'old_opcode_count':145,'new_opcode_count':146,'append':'SLOT_GROUP_INIT145'}
    source=(ROOT/'src/xir/xxir_ops.def').read_text(encoding='utf-8')
    names=re.findall(r'^XR_XIR_OP\((\w+),',source,re.M)
    assert names==model['previous_opcode_names']+['SLOT_GROUP_INIT'],'existing opcode ordinal changed'
    headers={};history=[];entries=[];manifest=[]
    for index,record in enumerate(model['records']):
        old=old_bytes(record);reproduced=fixed_packet(record,64);assert reproduced==old,record['name']
        current=fixed_packet(record,65);assert current[64:]==old[64:]
        headers.setdefault(record['header'],[]).append(array(record['name'],current))
        history.append(array('previous64_packet_'+str(index),old))
        entries.append('    {'+record['name']+',sizeof('+record['name']+'),previous64_packet_'+str(index)+',sizeof(previous64_packet_'+str(index)+'),'+('true' if record['reader_positive'] else 'false')+'},')
        manifest.append({'name':record['name'],'reader_positive':record['reader_positive'],'header':record['header'],'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest(),'body_sha256':hashlib.sha256(current[64:]).hexdigest(),'old64_sha256':hashlib.sha256(old).hexdigest()})
    for rel,contents in headers.items():
        guard=re.sub(r'[^A-Za-z0-9]','_',Path(rel).name.upper())
        content='/* Complete packets independently framed by named family roles. */\n#ifndef '+guard+'\n#define '+guard+'\n'+''.join(contents)+'#endif\n'
        target=ROOT/rel
        if args.write:target.write_text(content,encoding='utf-8',newline='\n')
        else:assert target.read_text(encoding='utf-8')==content,rel
    includes=''.join('#include "'+str(Path(rel).relative_to('tests/unit/xir')).replace('\\','/')+'"\n' for rel in headers)
    content='/* Historical complete64 packets and explicit reader or writer-only roles. */\n'+includes+''.join(history)
    content+='typedef struct Semantic65Vector {const uint8_t *current;size_t current_size;const uint8_t *previous;size_t previous_size;bool reader_positive;} Semantic65Vector;\n'
    content+='static const Semantic65Vector semantic65_vectors[]={\n'+'\n'.join(entries)+'\n};\n'
    target=DIRECTORY/'semantic65_packet_cases.h'
    if args.write:target.write_text(content,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==content
    print(json.dumps({'packets':len(manifest),'reader_positives':sum(x['reader_positive'] for x in manifest),'writer_only':sum(not x['reader_positive'] for x in manifest),'headers':len(headers),'current_and_history':manifest},indent=2))

if __name__=='__main__':main()
