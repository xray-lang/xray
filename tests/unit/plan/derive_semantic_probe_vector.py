import ast
import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HISTORY = '373f65dbed4fbe61bb8cbb9d2fee582de6923e5f'
NONE = 0xffffffff
OLD = '744f387451b4a17da7cb52f034e3ff24cb6b0f27856f0ec7a3dbdaef66cf6bb5'

def u64(n): return struct.pack('<Q', n & 0xffffffffffffffff)
def s(v):
    b = (v or '').encode()
    return u64(len(b)) + b
def sha(b): return hashlib.sha256(b).digest()
def sid(k): return sha(b'xray-entity-id-v2\0' + s(k))[:16]
def pb(b): return u64(len(b)) + b
def ps(v):
    b=(v or '').encode()
    return u64(len(b)) + pb(b)
def read(path): return (ROOT/path).read_text(encoding='utf-8')

def split_fields(row):
    values = []
    begin = 0
    quoted = escaped = False
    for i, c in enumerate(row):
        if escaped: escaped = False; continue
        if c == '\\' and quoted: escaped = True; continue
        if c == '"': quoted = not quoted
        if c == ',' and not quoted:
            values.append(row[begin:i]); begin = i + 1
    values.append(row[begin:])
    return [v.strip() for v in values]

def split_row(row):
    values = split_fields(row)
    result = []
    for v in values:
        v = v.strip()
        if v.startswith('"'): result.append(ast.literal_eval(v))
        elif v in ('true','false'): result.append(v == 'true')
        elif v.startswith('XR_STDLIB_TARGET_LEAF_'):
            result.append({'XR_STDLIB_TARGET_LEAF_NONE':0, 'XR_STDLIB_TARGET_LEAF_I64_GETPID':1}[v])
        else:
            n = 0
            for part in v.split('|'):
                part = part.strip()
                if part.startswith('XR_CAP_'):
                    caps = ['COROUTINE','CHANNEL','EXCEPTION','NATIVE','EXTERN','OBJECTS','DEEP_COPY','INSTANCEOF','SYS_THREAD','SCOPE','TIMER','NETPOLL','TASK','ATOMIC','WORK_QUEUE','RESULT_GROUP','COUNTDOWN_LATCH','SEMAPHORE','EVENT_COUNT','GENERATOR','STACKTRACE','PARALLEL']
                    n |= 1 << caps.index(part[7:])
                else: n |= int(part.rstrip('uU'),0)
            result.append(n)
    return result

def operation_registry():
    header=read('src/plan/semantic/xr_semantic_ops_gen.h')
    owners=read('src/shared/xr_semantic_owner_ids_gen.h')
    values={name:int(n,0) for name,n in re.findall(r'#define\s+(\w+)\s+UINT(?:32|64)_C\((0x[0-9a-f]+|\d+)\)',owners)}
    for i,n in enumerate(['DECLARATIVE_PRIMITIVE','SHARED_SEMANTIC_KERNEL','CAPABILITY_PROVIDER','GENERATED_SPECIALIZATION']): values['XR_SEM_OWNER_'+n]=i
    for i,n in enumerate(['OWNED','BORROWED','NONE','CALL_RESULT']): values['XR_SEM_RESULT_OWNERSHIP_'+n]=i
    for i,n in enumerate(['CONSUME','BORROW','STORED_VALUE','METHOD_ARGS','PASS']): values['XR_SEM_OWN_USE_'+n]=i
    values['UINT8_MAX']=255
    values['XR_SEMANTIC_OP_ARITY_VARIADIC']=255
    for i,n in enumerate(['ALLOCATES','MAY_SUSPEND','MAY_THROW','MEMORY_READ','MEMORY_WRITE','RELEASES','RETAINS','SIDE_EFFECT']): values['XR_SEM_EFFECT_'+n]=1<<i
    def number(token):
        if '|' in token:
            result=0
            for part in token.split('|'): result|=number(part.strip())
            return result
        macro=re.fullmatch(r'UINT(?:32|64)_C\((.*?)\)',token)
        if macro: return int(macro.group(1),0)
        if token in values: return values[token]
        return int(token,0)
    owner_fingerprint=re.search(r'XR_SEMANTIC_OWNER_REGISTRY_FINGERPRINT "([0-9a-f]+)"',owners).group(1)
    assert owner_fingerprint=='3a3ed5f5568c62b050ec0bb1502c2bed82c0c643cc135d9592d43c7a8db8caee'
    xi=read('src/ir/xi.h')
    xi=xi[xi.index('XI_CONST = 0'):xi.index('} XiOp;')]
    xi=re.sub(r'/\*.*?\*/|//[^\n]*','',xi,flags=re.S)
    opcodes={}
    next_opcode=0
    for declaration in xi.split(','):
        match=re.search(r'XI_(\w+)(?:\s*=\s*(\d+))?',declaration)
        if not match: continue
        if match.group(2): next_opcode=int(match.group(2))
        opcodes[match.group(1)]=next_opcode
        next_opcode+=1
    rows=[]
    for line in header.splitlines():
        line=line.strip()
        if not line.startswith('X('): continue
        tokens=split_fields(line[2:line.rfind(')')])
        assert len(tokens)==28
        strings=[ast.literal_eval(tokens[i]) for i in [1,3,8,12,13,14,16,17,18,19,20,21,22,25,26,27]]
        numbers=[number(tokens[i]) for i in [4,5,6,7,24]]+[opcodes[tokens[0]]]+[number(tokens[i]) for i in [2,9,10,11,15,23]]
        rows.append(dict(name=tokens[0],strings=strings,numbers=numbers))
    rows.sort(key=lambda row:row['numbers'][5])
    assert [row['numbers'][5] for row in rows]==list(range(233))
    raw=b'xray-semantic-operation-registry-v2\0'+s(owner_fingerprint)+u64(len(rows))
    for row in rows: raw+=b''.join(s(v) for v in row['strings'])+b''.join(u64(v) for v in row['numbers'])
    fp=sha(raw).hex()
    assert len(rows)==233 and fp=='3ad410995d50cb11c5cc1475a3279e15c7fbaf43c3482a35a8f3f89b0d5ce629',(len(rows),fp)
    return dict(owner_fingerprint=owner_fingerprint,fingerprint=fp,rows=rows,preimage=raw.hex())

def registry(old):
    header = read('src/stdlib/xstdlib_defs_generated.h')
    legacy = json.loads((Path(__file__).with_name('semantic_probe_legacy_registry.json')).read_text(encoding='utf-8'))
    assert legacy['source_commit'] == HISTORY and legacy['schema'] == 2
    def rows(typ, name):
        struct_body = header.split('typedef struct ' + typ + ' {',1)[1].split('}',1)[0]
        fields = re.findall(r'(\w+)\s*;', struct_body)
        body = header.split(name + '[] = {',1)[1].split('\n};',1)[0]
        result = []
        for line in body.splitlines():
            line = line.strip()
            if not line.startswith('{'): continue
            vals = split_row(line[1:line.rfind('}')])
            assert len(vals)==len(fields), (len(vals), fields, line[:70])
            result.append(dict(zip(fields, vals)))
        return result
    functions = legacy['functions'] if old else rows('XrStdlibDefEntry','xr_stdlib_def_entries')
    natives = legacy['natives'] if old else rows('XrStdlibNativeClassDefEntry','xr_stdlib_native_class_def_entries')
    version = 2 if old else 4
    names = ['module','name','signature','vm','vm_binding','vm_ifdef','aot','arg_spec','ret','aot_enum','link_object','define','layer','aot_kind','return_ownership']
    if not old: names += ['provider_contract_key','provider_operation_key','suspension_kind']
    nums = ['runtime_capabilities','argc'] + (['target_leaf'] if old else []) + ['aot_direct']
    raw = b'xray-stdlib-definition-registry-v%d\0' % version + u64(len(functions))
    for row in functions:
        raw += b''.join(s(row[n]) for n in names) + b''.join(u64(row[n]) for n in nums)
    raw += u64(len(natives))
    native_names = ['module','name','super_slot','core_slot','native_body_expr','flags','builtin_kind','source_wrapper','source_storage_field']
    for row in natives:
        raw += sha(b'xray-stdlib-native-class-v1\0' + b''.join(s(row[n]) for n in native_names))
    return dict(functions=functions, natives=natives, fingerprint=sha(raw).hex(), preimage=raw.hex())

def fixture(schema, registry_fingerprint, operation_fingerprint):
    # The literal fixture is bool true, string owned-by-plan, int 42, then return int.
    types = []
    for kind,rep,flags in [(0,0,0),(3,255,0),(2,255,80)]:
        key = f'type-v3:{kind}:0:0:0:0:0:0:0:0:{rep}:0:'
        types.append(dict(id=sid(key),key=key,kind=kind,rep=rep,flags=flags))
    types.sort(key=lambda t:t['id'])
    ti = {t['kind']:i for i,t in enumerate(types)}
    fn_key = f"function-v3:parent=module-root:ordinal=0:name=14:artifact_probe:source-class=none:member=none:source-kind=0:return={types[ti[0]]['id'].hex()}:params=0:effects=0:caps=0:flags=0"
    fn_id = sid(fn_key)
    ops=[]
    for i,(kind,imm,ck,text) in enumerate([(3,1,4,''),(2,0,6,'owned-by-plan'),(0,42,2,'')]):
        key = fn_key+f'/op:{i}:CONST'
        ops.append(dict(id=sid(key),key=key,result_type=ti[kind],immediate=imm,constant_kind=ck,text=text))
    owner_key = f"owner-v2:{ops[1]['id'].hex()}:value=1"
    owner_id = sid(owner_key)
    entities=[]
    def entity(kind,parent,subject=NONE,subject_kind=0,suffix=''):
        p = 'none' if parent==NONE else entities[parent]['id'].hex()
        key = f'entity-v1:schema={schema}:kind={kind}:parent={p}'+suffix
        e = dict(id=sid(key),key=key,parent=parent,subject=subject,ordinal=0,kind=kind,subject_kind=subject_kind,flags=0)
        entities.append(e)
        return len(entities)-1
    authority = 'memory-module-v1:id=24:semantic-plan-fixture-v1'
    pkg = entity(0,NONE,suffix=':authority='+str(len(authority))+':'+authority)
    mod = entity(1,pkg,suffix=':name=21:semantic_plan_fixture:identity='+str(len(authority))+':'+authority)
    for i,t in enumerate(types): entity(3,mod,i,1,':type='+t['id'].hex())
    decl = entity(2,mod,0,2,':function='+fn_id.hex()+':evidence=0')
    fn = entity(6,decl,0,2,':function='+fn_id.hex())
    for i,o in enumerate(ops): entity(9,fn,i,5,':operation='+o['id'].hex())
    entity(11,fn,0,6,':owner='+owner_id.hex())
    for domain in range(1,7): entity(13,mod,domain,7,f':storage-domain={domain}')
    original = entities[:]
    entities.sort(key=lambda e:e['id'])
    for e in entities:
        if e['parent']!=NONE: e['parent']=entities.index(original[e['parent']])
    raw=b'xray-semantic-plan-v23\0'+u64(schema)
    raw+=pb(bytes.fromhex(operation_fingerprint))+pb(bytes.fromhex(registry_fingerprint))
    counts=[3,0,0,1,0,0,1,3,0,0,0,0,3,len(entities)]
    raw+=b''.join(u64(n) for n in counts)
    for e in entities:
        raw+=pb(e['id'])+ps(e['key'])+b''.join(u64(e[n]) for n in ['parent','subject','ordinal','kind','subject_kind','flags'])
    for t in types:
        raw+=pb(t['id'])+ps(t['key'])+pb(bytes(16))+ps('')
        raw+=u64(t['kind'])+u64(0)+u64(NONE)+pb(bytes(16))
        raw+=b''.join(u64(n) for n in [0,0,0,0,0,0,t['rep'],t['flags'],0,0])
    fn_fields=[ti[0],NONE,0,0,0,0,0,0,1,0,3,0,0,NONE,65535,65535,0,0,0,0,0]
    fn_names='return_type parent parameter_begin parameter_count child_count capture_begin capture_count block_begin block_count value_begin value_count semantic_effects capability_mask source_class source_member_ordinal return_parameter return_provenance source_kind flags is_module_initializer carries_coroutine_ops'.split()
    if schema >= 51: fn_fields.append(0)
    if schema >= 51: fn_names.append('is_external_entry')
    raw+=pb(fn_id)+ps(fn_key)+ps('artifact_probe')+b''.join(u64(n) for n in fn_fields)
    block_key=fn_key+'/block:0'
    block_fields=[0,0,3,0,0,2,NONE,NONE,2,0]
    block_names='function operation_begin operation_count predecessor_begin predecessor_count kind successor_0 successor_1 control_value source_line'.split()
    raw+=pb(sid(block_key))+ps(block_key)+b''.join(u64(n) for n in block_fields)
    for i,o in enumerate(ops):
        raw+=pb(o['id'])+pb(bytes(16))+ps(o['key'])+ps('')
        first=[0,0,i,o['result_type'],0,0,0,0,0,0,0,0,0]
        first_names='function block result_value result_type operand_begin operand_count opcode metadata_begin metadata_count auxiliary_kind import_resolution effects source_line'.split()
        second=[0,0,0,0,0,o['immediate'],i,NONE]
        second_names='source_start_line source_start_column source_end_line source_end_column source_discriminator semantic_immediate constant callable_function'.split()
        evidence=[0,0,0,0,0,0,0,NONE]
        last=[1,0,0,0,0,0,65535,65535,3,1,NONE,NONE,65535,65535,0,0,0,0,0,0,0,0]
        last_names='ownership_use result_ownership transfer_mode parameter_mode parameter_ownership flags result_alias_operand return_parameter return_provenance return_complete view_source_value view_element_type view_source_operand view_source_parameter intrinsic_kind view_origin view_capability view_lifetime view_complete array_element_storage array_hof_kind array_result_element_storage'.split()
        raw+=b''.join(u64(n) for n in first)
        raw+=ps('')+b''.join(u64(n) for n in second+evidence+last)
        o['fields']=dict(zip(first_names+second_names+['evidence_'+str(n) for n in range(8)]+last_names,first+second+evidence+last))
        o['fields'].update(allocation_id=bytes(16),allocation_key='',source_file='')
    for o in ops:
        raw+=b''.join(u64(n) for n in [o['result_type'],o['constant_kind'],o['immediate'],0])+ps(o['text'])
    raw+=b''.join(u64(n) for n in [1,0,1,0])
    raw+=pb(owner_id)+ps(owner_key)+b''.join(u64(n) for n in [0,1,10,10,0,0])
    raw+=b''.join(u64(n) for n in [0,0,NONE,0,0,10,10,0])
    def serial(x):
        if isinstance(x,bytes): return x.hex()
        if isinstance(x,list): return [serial(v) for v in x]
        if isinstance(x,dict): return {k:serial(v) for k,v in x.items()}
        return x
    count_names='type source_class source_method function parameter capture block operation call_target dependency source_export edge constant entity'.split()
    function=dict(id=fn_id,key=fn_key,name='artifact_probe',fields=dict(zip(fn_names,fn_fields)))
    block=dict(id=sid(block_key),key=block_key,fields=dict(zip(block_names,block_fields)))
    constants=[dict(type=o['result_type'],kind=o['constant_kind'],integer=o['immediate'],float_bits=0,string=o['text']) for o in ops]
    ownership=dict(counts=dict(owner=1,event=0,edge_state=1,loop_invariant=0),owner=dict(id=owner_id,key=owner_key,function=0,origin_value=1,initial_state=10,exit_state=10,return_provenance=0,flags=0),edge_state=dict(owner=0,block=0,successor=NONE,entry_balance=0,exit_balance=0,entry_state=10,exit_state=10,flags=0))
    for t in types:
        t['fields']=dict(source_enum_identity=bytes(16),source_enum_key='',kind=t['kind'],builtin_type=0,source_class=NONE,source_class_identity=bytes(16),child_begin=0,aggregate_extent=0,aggregate_align=0,enum_layout_id=0,child_count=0,enum_member_count=0,scalar_rep=t['rep'],flags=t['flags'],enum_flags=0,reserved_enum=0)
    return serial(dict(fingerprint=sha(raw).hex(), preimage=raw.hex(), counts=dict(zip(count_names,counts)), auxiliary_counts=dict(type_child=0,predecessor=0,operand=0,metadata=0,program_provenance_schema=0),types=types,function=function,block=block,operations=ops,constants=constants,entities=entities,ownership=ownership))



def call_target_vectors():
    def identity(key): return sid(key).hex()
    integer_key = 'type-v3:0:0:0:0:0:0:0:0:0:0:0:'
    unit_key = 'type-v3:17:0:0:0:0:0:0:0:0:255:0:'
    bool_key = 'type-v3:3:0:0:0:0:0:0:0:0:255:0:'
    callable_key = ('type-v3:13:0:0:0:0:0:0:0:0:0:0:fn:0:0:0:0:0;ret:' +
                    integer_key + ';view-count:0')
    semaphore_key = 'type-v3:11:0:39:0:0:0:0:0:0:255:0:;named:9:Semaphore[0]'
    channel_key = 'type-v3:8:0:0:0:0:0:0:0:0:255:0:;element:' + bool_key
    keys = dict(integer=integer_key, unit=unit_key, boolean=bool_key,
                callable=callable_key, historical_semaphore=semaphore_key, channel=channel_key)
    types = {name:dict(key=key,id=identity(key)) for name,key in keys.items()}
    def function(name, parent='module-root', ordinal=0, result='integer', parameters=()):
        return (f'function-v3:parent={parent}:ordinal={ordinal}:name={len(name.encode())}:{name}'
                ':source-class=none:member=none:source-kind=0:return=' + types[result]['id'] +
                f':params={len(parameters)}' + ''.join(
                    f':p{i}:mode=0:type=' + types[p]['id'] for i,p in enumerate(parameters)) +
                ':effects=0:caps=0:flags=0')
    direct_root = function('direct_call_target_root')
    direct_child = function('direct_call_target_child', identity(direct_root))
    indirect = function('indirect_callable_probe', parameters=('callable',))
    namespace_root = function('native_namespace_root', result='unit')
    namespace = function('native_namespace_caller', identity(namespace_root), result='unit')
    old_builtin = function('builtin_instance_yieldable_probe', result='unit',
                           parameters=('historical_semaphore',))
    builtin = function('builtin_instance_yieldable_probe', result='boolean', parameters=('channel',))
    shared_root = function('shared_direct_root')
    shared_child = function('shared_direct_target', identity(shared_root))
    shared = function('shared_direct_caller', identity(shared_root), ordinal=1)
    fixtures = [
        ('direct',49,'c82178225e60725e31ba52a2f2ec2363','v3',
         direct_root+'/op:2:CALL', ':function='+identity(direct_child)+':kind=1', None),
        ('indirect',49,'a3e081cd6dce721b370aa03fc5e44e9b','v3',
         indirect+'/op:2:CALL', ':callable-type='+types['callable']['id']+':kind=4', None),
        ('namespace',45,'a21ea08f53a086fdb6045813f6b8ab10','v5',
         namespace+'/op:2:CALL_METHOD', ':native-namespace=time.sleep:kind=5',
         (namespace+'/op:2:CALL_METHOD', ':native-namespace=time.__sleep:kind=5')),
        ('builtin',45,'74f7bea0914ede2ff42b0d8057691b82','v6',
         old_builtin+'/op:1:CALL_METHOD',
         ':builtin-instance=Semaphore.acquire:type='+types['historical_semaphore']['id']+':kind=6',
         (builtin+'/op:2:CALL_METHOD',
          ':builtin-instance=Channel.recvOr:type='+types['channel']['id']+':kind=6')),
        ('shared',47,'d82bc974ffe5d9eaeb790d5291e797a3','v3',
         shared+'/op:1:CALL', ':function='+identity(shared_child)+':kind=1', None),
    ]
    result = dict(types=types, fixtures={})
    for name,old_schema,old_id,version,operation,tail,replacement in fixtures:
        def vector(schema, op, suffix):
            key = f'call-target-{version}:schema={schema}:operation='+identity(op)+suffix
            return dict(schema=schema,operation_key=op,operation=identity(op),key=key,id=identity(key))
        old = vector(old_schema,operation,tail)
        assert old['id'] == old_id, (name,old)
        new_op,new_tail = replacement or (operation,tail)
        result['fixtures'][name] = dict(historical=old,current=vector(51,new_op,new_tail))
    export_root = function('net_init', result='unit')
    exported = function('writeBytes', identity(export_root), result='unit')
    exports = {}
    for schema in (47,51):
        key = f'source-export-v1:schema={schema}:name=10:writeBytes:function='+identity(exported)+':slot=0'
        exports[str(schema)] = dict(key=key,id=identity(key))
    assert exports['47']['id'] == 'e6c5f7ae334037299a1f5f282751a948'
    result['source_export'] = dict(root_key=export_root,function_key=exported,vectors=exports)
    return result



def dependency_fixture(schema, registry_fingerprint, operation_registry):
    assert schema == 51
    operation_fingerprint = operation_registry['fingerprint']
    integer='type-v3:0:0:0:0:0:0:0:0:0:0:0:'
    unit='type-v3:17:0:0:0:0:0:0:0:0:255:0:'
    callable=('type-v3:13:0:0:0:0:0:0:0:0:0:0:fn:0:0:0:0:0;ret:'+integer+';view-count:0')
    types=[dict(key=key,id=sid(key),kind=kind,rep=rep,flags=flags,children=children)
           for key,kind,rep,flags,children in [(integer,0,0,0,[]),(unit,17,255,0,[]),(callable,13,0,80,[integer])]]
    types.sort(key=lambda t:t['id'])
    ti={t['kind']:i for i,t in enumerate(types)}
    child_cursor=0
    for t in types:
        t['child_begin']=child_cursor;child_cursor+=len(t['children'])
    def fn(name,parent='module-root'):
        return (f'function-v3:parent={parent}:ordinal=0:name={len(name)}:{name}'
                ':source-class=none:member=none:source-kind=0:return='+sid(unit).hex()+
                ':params=0:effects=0:caps=0:flags=0')
    functions=[fn('net_init')]
    functions.append(fn('writeBytes',sid(functions[0]).hex()))
    opkeys=[functions[0]+'/op:0:CLOSURE_NEW',functions[0]+'/op:1:SET_SHARED',functions[1]+'/op:0:YIELD']
    alloc=opkeys[0]+'/allocation'
    owner='owner-v2:'+sid(opkeys[0]).hex()+':value=0'
    ownerid=sid(owner)
    entities=[]
    def entity(kind,parent,subject=NONE,subject_kind=0,suffix='',ordinal=0):
        p='none' if parent==NONE else entities[parent]['id'].hex()
        key=f'entity-v1:schema={schema}:kind={kind}:parent={p}'+suffix
        entities.append(dict(id=sid(key),key=key,parent=parent,subject=subject,
                             ordinal=ordinal,kind=kind,subject_kind=subject_kind,flags=0))
        return len(entities)-1
    authority='stdlib-module-v1:module=3:net:path=10:net/net.xr'
    package=entity(0,NONE,suffix=f':authority={len(authority)}:{authority}')
    module=entity(1,package,suffix=':name=3:net:identity='+str(len(authority))+':'+authority)
    moduleid=entities[module]['id']
    for i,t in enumerate(types):entity(3,module,i,1,':type='+t['id'].hex())
    fnentities=[]
    for i,fnkey in enumerate(functions):
        parent=module if i==0 else fnentities[0]
        decl=entity(2,parent,i,2,':function='+sid(fnkey).hex()+':evidence=0')
        f=entity(6,decl,i,2,':function='+sid(fnkey).hex());fnentities.append(f)
        if i:entity(7,f,i,2,':function='+sid(fnkey).hex())
    for i,opkey in enumerate(opkeys):
        p=entity(9,fnentities[0 if i<2 else 1],i,5,':operation='+sid(opkey).hex())
        if i==0:entity(10,p,i,5,':allocation='+sid(alloc).hex())
    entity(11,fnentities[0],0,6,':owner='+ownerid.hex())
    for d in range(1,7):entity(13,module,d,7,f':storage-domain={d}')
    entity(14,fnentities[1],2,5,':state=1:operation='+sid(opkeys[2]).hex(),ordinal=1)
    original=entities[:];entities.sort(key=lambda x:x['id'])
    for record in entities:
        if record['parent']!=NONE:record['parent']=entities.index(original[record['parent']])
    counts=[3,0,0,2,0,0,2,3,0,0,1,0,0,len(entities)]
    raw=b'xray-semantic-plan-v23\0'+u64(schema)+pb(bytes.fromhex(operation_fingerprint))+pb(bytes.fromhex(registry_fingerprint))
    raw+=b''.join(u64(n) for n in counts)
    for record in entities:
        raw+=pb(record['id'])+ps(record['key'])+b''.join(u64(record[n]) for n in ['parent','subject','ordinal','kind','subject_kind','flags'])
    for t in types:
        raw+=pb(t['id'])+ps(t['key'])+pb(bytes(16))+ps('')
        raw+=u64(t['kind'])+u64(0)+u64(NONE)+pb(bytes(16))
        raw+=b''.join(u64(n) for n in [t['child_begin'],0,0,0,len(t['children']),0,t['rep'],t['flags'],0,0])
    raw+=u64(ti[0])
    fnfields=[
        [ti[17],NONE,0,0,1,0,0,0,1,0,2,0,0,NONE,65535,65535,0,0,0,0,0],
        [ti[17],0,0,0,0,0,0,1,1,2,1,0,0,NONE,65535,65535,0,0,0,0,1],
    ]
    for i,key in enumerate(functions):
        raw+=pb(sid(key))+ps(key)+ps(['net_init','writeBytes'][i])
        raw+=b''.join(u64(n) for n in fnfields[i]+([0] if schema>=51 else []))
    for i,key in enumerate(functions):
        block=key+'/block:0'
        fields=[i,0 if i==0 else 2,2 if i==0 else 1,0,0,2,NONE,NONE,NONE if i==0 else 2,0]
        raw+=pb(sid(block))+ps(block)+b''.join(u64(n) for n in fields)
    opfields=[]
    # Declared opcode numbers are input facts; no produced plan is read.
    declared = {row['name']:row['numbers'][5] for row in operation_registry['rows']}
    for i,key in enumerate(opkeys):
        f=0 if i<2 else 1;b=f
        first=[f,b,i,ti[13] if i==0 else ti[17],0 if i<2 else 1,1 if i==1 else 0,
               declared[['CLOSURE_NEW','SET_SHARED','YIELD'][i]],0 if i<2 else 2,2 if i==1 else 0,0,0,[145,144,130][i],0]
        second=[0,0,0,0,0,0,NONE,1 if i==0 else NONE]
        evidence=[0]*7+[NONE]
        last=[0,0 if i==0 else 2,0,0,0,[17,17,5][i],65535,65535,1 if i==0 else 0,1 if i==0 else 0,NONE,NONE,65535,65535,0,0,0,0,0,0,0,0]
        raw+=pb(sid(key))+pb(sid(alloc) if i==0 else bytes(16))+ps(key)+ps(alloc if i==0 else '')
        raw+=b''.join(u64(n) for n in first)+ps('')+b''.join(u64(n) for n in second+evidence+last)
        opfields.append(dict(first=first,second=second,last=last))
    export=f'source-export-v1:schema={schema}:name=10:writeBytes:function='+sid(functions[1]).hex()+':slot=0'
    raw+=pb(sid(export))+ps(export)+ps('writeBytes')+pb(sid(functions[1]))
    raw+=b''.join(u64(n) for n in [1,NONE,0,1])+pb(bytes(3))
    raw+=b''.join(u64(n) for n in [0,ti[13],65535,0,0,1,0,0,0,0,0,0])
    raw+=ps('source-export-v1')+ps('writeBytes')
    raw+=b''.join(u64(n) for n in [1,2,1,0])
    raw+=pb(ownerid)+ps(owner)+b''.join(u64(n) for n in [0,0,2,9,0,1])
    for index,(op,kind,delta,state) in enumerate([(0,0,1,2),(1,7,65535,5)]):
        key=f'ownership-event-v4:{ownerid.hex()}:{sid(opkeys[op]).hex()}:0:{NONE}:{kind}:0:{index}'
        raw+=pb(sid(key))+ps(key)+b''.join(u64(n) for n in [0,op,0,NONE,delta,kind,state,0])
    raw+=b''.join(u64(n) for n in [0,0,NONE,0,0,9,9,0])
    fingerprint=sha(raw).hex()
    dep=f'dependency-v1:schema={schema}:path={len(authority)}:{authority}:module='+moduleid.hex()+':semantic='+fingerprint
    callerroot=fn('http_init');caller=fn('_serverWriteAll',sid(callerroot).hex())
    callkey=caller+'/op:1:CALL_METHOD'
    target=f'call-target-v4:schema={schema}:operation='+sid(callkey).hex()+':dependency='+sid(dep).hex()+':export='+sid(export).hex()+':function='+sid(functions[1]).hex()+':kind=3'
    return dict(schema=schema,fingerprint=fingerprint,preimage=raw.hex(),
                dependency_key=dep,dependency_id=sid(dep).hex(),export_key=export,export_id=sid(export).hex(),
                target_key=target,target_id=sid(target).hex(),opfields=opfields,counts=counts)

def main():
    import argparse
    parser=argparse.ArgumentParser(description='Independently frame the SemanticPlan probe fixture')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--test-source',type=Path)
    args=parser.parse_args()
    globals()['ROOT'] = args.root.resolve()
    guard=read('src/plan/semantic/xr_semantic_ids.h')
    assert '#define XR_SEMANTIC_SCHEMA_VERSION UINT32_C(51)' in guard
    guard=read('src/plan/semantic/xr_program_semantic_closure.h')
    assert '#define XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION UINT32_C(10)' in guard
    operation=operation_registry()
    records={'operation_registry':operation,'call_targets':call_target_vectors()}
    for old,schema in [(True,49),(False,51)]:
        reg=registry(old)
        plan=fixture(schema,reg['fingerprint'],operation['fingerprint'])
        records[str(schema)]=dict(registry=reg,plan=plan)
        if old: assert plan['fingerprint']==OLD
        print('PASS schema',schema,'registry',reg['fingerprint'],'plan',plan['fingerprint'])
    dependency = dependency_fixture(51, records['51']['registry']['fingerprint'], operation)
    records['source_export_dependency'] = dependency
    test = (args.test_source.read_text(encoding='utf-8') if args.test_source
            else read('tests/unit/plan/test_semantic_plan.c'))
    assert OLD in test and records['51']['plan']['fingerprint'] in test
    for vector in records['call_targets']['fixtures'].values():
        assert vector['current']['id'] in test
    for name in ['fingerprint','dependency_id','export_id','target_id']:
        assert dependency[name] in test
    print('PASS source-export dependency', dependency['dependency_id'], 'target', dependency['target_id'])
    args.output.write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__': main()
