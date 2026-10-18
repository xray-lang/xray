"""Full original aggregate programs, with independent place and value models."""
from pathlib import Path
import hashlib,math,json
from dataclasses import dataclass
import independent_observation_format

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/010-9-generics.md': '246757d531f91e2d19d2cd18353b5ec8addf05d309e5489c10da37e48be81226',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'src/shared/xr_float_fmt.h': 'dc3f35e72dd96729e5948f98b8103810d1362bad8cec207aabf93db1352f983b',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'stdlib/types/set.xr': '6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f',
 'tests/diff/cases/semantics/collections/enum_payload_equality.xr': '79b7d47e86117c564bd7a3d983d21ede97912f8be3bd56a3e3ba5c9ccdac499d',
 'tests/diff/cases/semantics/collections/map_build_in_fn.xr': 'ba5bd40a93ea17bcaa3bcb20996c64f14b88cfed61aa056fab2b5b4aa75dfd62',
 'tests/diff/cases/semantics/collections/map_float_key_identity.xr': '40c2e61b4bed6b0b7cbcd09e24a31e485abccca8f49afbe38cc5ed207cfd61e4',
 'tests/diff/cases/semantics/collections/map_key_contextual_literal.xr': '0d5432c76859a467f9585fc05322071152f91bb62c87e23e17ced1327591d55d',
 'tests/diff/cases/semantics/collections/span_active_borrow_branch_nll_shared_core.xr': '61b5833e1f1788a0ca6e7d6643de29b6ee432f4f8ccc89478be78a94e8498090',
 'tests/diff/cases/semantics/collections/span_active_borrow_disjoint_owner_shared_core.xr': '07678352241e846fb01f5ac30e967a3913d95dde223dcd653639c267de444d81',
 'tests/diff/cases/semantics/collections/span_active_borrow_nested_owner_shared_core.xr': '369ec6f38f9b306c8557080c0303d15540ae6c890c5b1749c19d045acb34e41b',
 'tests/diff/cases/semantics/destructure/object_rename.xr': '156165a34d0e7f59f74ce707aace3b34a6e5ba597cdd88e9c8aa7a3f8bb0a4aa',
 'tests/diff/cases/semantics/destructure/tuple_fixed_width_unsigned.xr': '4510495f559019208e36689626455b788c63e8b7bd40dbc4b0885a5aecd80dc1',
 'tests/diff/cases/semantics/types/object_field_invariance.xr': 'a947df2ec9826427835313a9548f40e05aba1552c26af399780d32c9fa59fe4c',
 'tests/diff/cases/semantics/types/object_optional_sparse_fields.xr': '3c7c3d09253223b5bba0c9dae2794434c5e6a3786e463f619bd3b457d3b51d33',
 'tests/diff/cases/semantics/types/union_as_conversion.xr': 'b21cb6138b0f39e09af00a9bdc7b58c4c10749e61d9093a8aac1e4603f915bc7',
 'tests/diff/cases/semantics/types/union_member_selection.xr': 'e5f4ca0e68250a367053ed5fdab18ae1f6f9b74e9468001a942ccd08e95c29a2',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/010-9-generics.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'src/shared/xr_float_fmt.h',
 'stdlib/types/array.xr',
 'stdlib/types/map.xr',
 'stdlib/types/set.xr',
 'tests/diff/cases/semantics/collections/enum_payload_equality.xr',
 'tests/diff/cases/semantics/collections/map_build_in_fn.xr',
 'tests/diff/cases/semantics/collections/map_float_key_identity.xr',
 'tests/diff/cases/semantics/collections/map_key_contextual_literal.xr',
 'tests/diff/cases/semantics/collections/span_active_borrow_branch_nll_shared_core.xr',
 'tests/diff/cases/semantics/collections/span_active_borrow_disjoint_owner_shared_core.xr',
 'tests/diff/cases/semantics/collections/span_active_borrow_nested_owner_shared_core.xr',
 'tests/diff/cases/semantics/destructure/object_rename.xr',
 'tests/diff/cases/semantics/destructure/tuple_fixed_width_unsigned.xr',
 'tests/diff/cases/semantics/types/object_field_invariance.xr',
 'tests/diff/cases/semantics/types/object_optional_sparse_fields.xr',
 'tests/diff/cases/semantics/types/union_as_conversion.xr',
 'tests/diff/cases/semantics/types/union_member_selection.xr',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_PROGRAMS = {'enum_payload_equality': 'tests/diff/cases/semantics/collections/enum_payload_equality.xr',
 'map_build_in_fn': 'tests/diff/cases/semantics/collections/map_build_in_fn.xr',
 'map_float_key_identity': 'tests/diff/cases/semantics/collections/map_float_key_identity.xr',
 'map_key_contextual_literal': 'tests/diff/cases/semantics/collections/map_key_contextual_literal.xr',
 'object_field_invariance': 'tests/diff/cases/semantics/types/object_field_invariance.xr',
 'object_optional_sparse_fields': 'tests/diff/cases/semantics/types/object_optional_sparse_fields.xr',
 'object_rename': 'tests/diff/cases/semantics/destructure/object_rename.xr',
 'span_active_borrow_branch_nll_shared_core': 'tests/diff/cases/semantics/collections/span_active_borrow_branch_nll_shared_core.xr',
 'span_active_borrow_disjoint_owner_shared_core': 'tests/diff/cases/semantics/collections/span_active_borrow_disjoint_owner_shared_core.xr',
 'span_active_borrow_nested_owner_shared_core': 'tests/diff/cases/semantics/collections/span_active_borrow_nested_owner_shared_core.xr',
 'tuple_fixed_width_unsigned': 'tests/diff/cases/semantics/destructure/tuple_fixed_width_unsigned.xr',
 'union_as_conversion': 'tests/diff/cases/semantics/types/union_as_conversion.xr',
 'union_member_selection': 'tests/diff/cases/semantics/types/union_member_selection.xr'}
_NAMES=frozenset({'span_active_borrow_branch_nll_shared_core','span_active_borrow_disjoint_owner_shared_core',
 'span_active_borrow_nested_owner_shared_core','object_rename','tuple_fixed_width_unsigned',
 'object_field_invariance','object_optional_sparse_fields','union_as_conversion','union_member_selection',
 'map_build_in_fn','map_float_key_identity','map_key_contextual_literal','enum_payload_equality'})

_ENUM_PROGRAMS = {"enum_type_name":"tests/diff/cases/semantics/types/enum_type_name.xr","enum_value_aot_identity":"tests/diff/cases/semantics/oop/enum_value_aot_identity.xr","adt_enum_payload_tostring":"tests/diff/cases/match/adt_enum_payload_tostring.xr"}
_ENUM_INPUTS = {"tests/diff/cases/semantics/types/enum_type_name.xr":"33809215199c7233e2e15356702dd1c32815e243be22c1f992812dc566a5fc43","tests/diff/cases/semantics/oop/enum_value_aot_identity.xr":"5d4091d8bd0fde0c52003f404bf8160ae5c9a3efa80c75b453fc40efb133d021","tests/diff/cases/match/adt_enum_payload_tostring.xr":"b9ea779e8affb3eb8f1f0db2748373f56d8f4947cee7e00c2da2a5b724abaa16"}
_PROGRAMS.update(_ENUM_PROGRAMS)
_NAMES = _NAMES | frozenset(_ENUM_PROGRAMS)
_INPUTS.update(_ENUM_INPUTS)
_REQUIRED_INPUTS = _REQUIRED_INPUTS + tuple(_ENUM_INPUTS)

def render(value):
    return independent_observation_format.render({'expected':value})


@dataclass(frozen=True)
class EnumType:
    module: str
    name: str
    variants: tuple
    declaration: int = 0

    def __post_init__(self):
        names=[name for name,fields in self.variants]
        if not self.module or not self.name or len(names)!=len(set(names)) or not names:
            raise ValueError('Enum requires one exact nominal declaration and unique variants')
        for name,fields in self.variants:
            if len({key for key,kind in fields})!=len(fields):
                raise ValueError('Enum payload field identities must be unique')

    def construct(self, variant, **values):
        candidates=[(ordinal,fields) for ordinal,(name,fields) in enumerate(self.variants) if name==variant]
        if len(candidates)!=1:
            raise ValueError('Unknown enum variant')
        ordinal,fields=candidates[0]
        if set(values)!={name for name,kind in fields}:
            raise ValueError('Enum payload must provide every exact declared field')
        ordered=[]
        for name,kind in fields:
            value=values[name]
            valid=(type(value) is int and -(1<<63)<=value<(1<<63)) if kind=='i64' else type(value) is str if kind=='string' else type(value) is EnumValue and value.type==kind
            if not valid:
                raise ValueError('Enum payload does not satisfy its exact declared type')
            ordered.append(value)
        return EnumValue(self,variant,ordinal,tuple(ordered))


@dataclass(frozen=True)
class EnumValue:
    type: EnumType
    name: str
    ordinal: int
    payload: tuple

    def __post_init__(self):
        if (type(self.type) is not EnumType or type(self.ordinal) is not int
                or not 0 <= self.ordinal < len(self.type.variants)
                or self.type.variants[self.ordinal][0] != self.name):
            raise ValueError('Enum value must keep its exact declared variant and ordinal')
        fields=self.type.variants[self.ordinal][1]
        if type(self.payload) is not tuple or len(self.payload)!=len(fields):
            raise ValueError('Enum value payload does not match its declared fields')
        for value,(name,kind) in zip(self.payload,fields):
            valid=(type(value) is int and -(1<<63)<=value<(1<<63)) if kind=='i64' else type(value) is str if kind=='string' else type(value) is EnumValue and value.type==kind
            if not valid:
                raise ValueError('Enum value payload has a different declared type')


def enum_type_name(value):
    if type(value) is not EnumValue:
        raise ValueError('A declaration or payload constructor is not an enum value')
    return value.type.name


def enum_text(value, nested=False):
    if type(value) is EnumValue:
        head=value.type.name+'.'+value.name
        return head+('('+', '.join(enum_text(item,True) for item in value.payload)+')' if value.payload else '')
    if type(value) is str:
        return json.dumps(value,ensure_ascii=False) if nested else value
    return render(value)


def enum_surface_traces():
    name_source=_ENUM_PROGRAMS['enum_type_name']
    color=EnumType(name_source,'Color',(('Red',()),('Green',())))
    shape=EnumType(name_source,'Shape',(('Circle',(('radius','i64'),)),('Point',())),1)
    names=[enum_type_name(color.construct('Red')),enum_type_name(color.construct('Green')),enum_type_name(shape.construct('Point'))]
    value_source=_ENUM_PROGRAMS['enum_value_aot_identity']
    color=EnumType(value_source,'Color',(('Red',()),('Green',()),('Blue',())))
    direction=EnumType(value_source,'Direction',(('North',()),('East',())),1)
    status=EnumType(value_source,'HttpStatus',(('OK',()),('NotFound',())),2)
    red,green,blue=[color.construct(name) for name in ('Red','Green','Blue')]
    outputs=['|'.join(enum_text(value) for value in (red,green,blue)),red.name,green.ordinal,blue.ordinal,enum_text(red)]
    middle=green
    outputs += [middle==color.construct('Green'),middle.name+':'+str(middle.ordinal)]
    north=direction.construct('North')
    outputs += [{'North':'N','East':'E'}[north.name],enum_text(north),{'OK':200,'NotFound':404}[status.construct('NotFound').name]]
    payload_source=_ENUM_PROGRAMS['adt_enum_payload_tostring']
    e_type=EnumType(payload_source,'E',(('A',(('value','i64'),)),('B',())))
    p_type=EnumType(payload_source,'P',(('Pair',(('first','i64'),('second','string'))),('Solo',())),1)
    w_type=EnumType(payload_source,'W',(('Wrap',(('value',e_type),)),('Empty',())),2)
    e=e_type.construct('A',value=7)
    values=['x='+enum_text(e),enum_text(e),'i='+enum_text(e),'c='+enum_text(e),enum_text(e_type.construct('B')),'b='+enum_text(e_type.construct('B'))]
    pair=p_type.construct('Pair',first=3,second='hi');values += [enum_text(pair),'p='+enum_text(pair)]
    wrap=w_type.construct('Wrap',value=e_type.construct('A',value=9));values += [enum_text(wrap),'w='+enum_text(wrap)]
    def describe(value):return 'g='+enum_text(value)
    values += [describe(e),describe(pair)]
    long=p_type.construct('Pair',first=42,second='a'*80);values += ['L='+enum_text(long),'M='+enum_text(long)]
    return {'enum_type_name':{'outputs':names,'enum_value_queries':3,'unexecuted_payload_variant_declaration_retained':True},
            'enum_value_aot_identity':{'outputs':outputs,'array_join_enum_values':3,'method_dispatch_calls':2,'fresh_same_variant_equality':True},
            'adt_enum_payload_tostring':{'outputs':values,'generic_describe_calls':2,'explicit_toString_runtime_calls':7,
                'string_conversion_calls':1,'interpolation_calls':6,'long_payload_UTF8_bytes':80,
                'generic_definition_display_permission_proved':False,'full_original_generic_body_retained':True}}

class BorrowLedger:
    def __init__(self):self.active=set()
    def begin(self,path):self.active.add(tuple(path));return tuple(path)
    def end(self,path):self.active.remove(tuple(path))
    def mutate(self,path):
        path=tuple(path)
        if any(path[:len(active)]==active or active[:len(path)]==path for active in self.active):
            raise ValueError('A live shared borrow prohibits mutation of an overlapping place')

def materialize(schema,fields):
    if set(fields)-set(schema):raise ValueError('Unexpected structural field')
    result={}
    for name,nullable in schema.items():
        if name not in fields and not nullable:raise ValueError('Missing required structural field')
        result[name]=fields.get(name)
    return result

def select_union(members,source_kind,value):
    if source_kind in members:return source_kind,value
    integers={'i8','i16','i32','i64','u8','u16','u32','u64'};floats={'f32','f64'}
    family=integers if source_kind in integers else floats if source_kind in floats else {source_kind}
    matches=[name for name in members if name in family]
    if len(matches)!=1:raise ValueError('No unique union member')
    return matches[0],float(value) if matches[0] in floats else value

def float_key(value):
    if math.isnan(value):return ('nan',)
    return ('f64',0.0 if value==0 else value)

def observations():
    rows={};events=[]
    for flag in [True,False]:
        values=[1];ledger=BorrowLedger();view=ledger.begin(('bytes',))
        if flag:events.append(len(values))
        ledger.end(view);ledger.mutate(('bytes',));values.append(2 if flag else 3)
        events.extend([len(values),len(values)])
    rows['span_active_borrow_branch_nll_shared_core']=events
    events=[]
    for root,left,right,replacement in [('box',1,9,10),('chunks',3,7,8)]:
        values={'left':[left],'right':[right]};ledger=BorrowLedger();view=ledger.begin((root,'left'))
        ledger.mutate((root,'right'));values['right']=[];values['right'].append(replacement)
        events.extend([len(values['left']),values['left'][0],len(values['right']),values['right'][0]])
        ledger.end(view)
    rows['span_active_borrow_disjoint_owner_shared_core']=events
    events=[]
    for path,first,second in [(('box','bytes'),7,9),(('chunks',0),3,4)]:
        values=[first];ledger=BorrowLedger();view=ledger.begin(path);events.extend([len(values),values[0]])
        ledger.end(view);ledger.mutate(path);values.append(second);events.append(len(values))
    rows['span_active_borrow_nested_owner_shared_core']=events

    user={'name':'Alice','age':30,'city':'Shanghai'};local_name,age=user['name'],user['age'];other_name,other_city=user['name'],user['city']
    rows['object_rename']=[local_name,age,other_name,other_city]
    product=0xF120000000000000*0x8D7EB76070A08AEC;hi,lo=product>>64,product%(1<<64)
    triple=(0,10000000000000000,100000000000000000);status,byte=('TupleStatus','Invalid'),7
    rows['tuple_fixed_width_unsigned']=[hi>>11,lo>>63,triple[1],triple[2],status==('TupleStatus','Invalid'),byte]
    page={'limit':True,'cursor':True};user_schema={'name':False,'age':True}
    def describe(fields):
        value=materialize(page,fields)
        return str(value['limit'] if value['limit'] is not None else -1)+'/'+(value['cursor'] if value['cursor'] is not None else '<none>')
    values=[describe({'limit':5,'cursor':None}),describe({'limit':None,'cursor':'abc'}),describe({})]
    for fields in [{'name':'Alice'},{'name':'Bob','age':3}]:
        value=materialize(user_schema,fields);values.append(value['name']+'/'+str(value['age'] if value['age'] is not None else -1))
    rows['object_field_invariance']=values
    schema={'required':False,'method':True,'headers':True,'timeoutMs':True}
    late=materialize(schema,{'required':'late','timeoutMs':25});middle=materialize(schema,{'required':'middle','headers':7})
    rows['object_optional_sparse_fields']=[late['required'],late['method'] is None,late['headers'] is None,late['timeoutMs'],middle['method'] is None,middle['headers'],middle['timeoutMs'] is None]
    a=select_union(['i64','string'],'i64',7);b=select_union(['i64','string'],'string','q');c=select_union(['i32','f64'],'i64',3)
    rows['union_as_conversion']=['a='+render(a[1])+' b='+render(b[1]),'c='+render(c[1])]
    cases=[('a',['i64','f64'],'i64',1),('b',['i64','f64'],'f64',1.0),('c',['i32','string'],'i64',7),
     ('d',['f32','string'],'f64',1.5),('e',['i32','string'],'string','s'),('widened',['i32','string'],'i16',3),('switched',['i64','f64'],'f64',2.5)]
    events=[]
    for name,members,kind,value in cases:
        selected,payload=select_union(members,kind,value)
        events.append(name+'='+render(payload)+''.join(' is '+member+'='+render(selected==member) for member in members))
    events.extend(['m1 '+select_union(['i64','f64'],'i64',3)[0],'m2 '+select_union(['i64','f64'],'f64',3.5)[0]])
    rows['union_member_selection']=events

    def squares(n):return {index:index*index for index in range(n)}
    first,second=squares(6),squares(3);rows['map_build_in_fn']=[len(first),first[0],first[3],first[5],sum(first.values()),len(second),second[2]]
    nan=float('nan');s={float_key(nan),float_key(nan)};events=['nan set size='+str(len(s)),'nan set contains='+render(float_key(nan) in s)]
    mapping={};mapping[float_key(nan)]='first';mapping[float_key(nan)]='second'
    events.extend(['nan map size='+str(len(mapping)),'nan map containsKey='+render(float_key(nan) in mapping),'nan map get='+mapping[float_key(nan)]])
    del mapping[float_key(nan)];events.append('nan map size after delete='+str(len(mapping)))
    mapping[float_key(0.0)]='pos';mapping[float_key(-0.0)]='neg'
    events.extend(['zero map size='+str(len(mapping)),'zero get='+mapping[float_key(0.0)],'neg zero get='+mapping[float_key(-0.0)],'0.0 == -0.0 -> '+render(0.0==-0.0),'nan == nan -> '+render(nan==nan)])
    rows['map_float_key_identity']=events
    mapping={};mapping[float(1)]='int-literal';events=['size after int literal='+str(len(mapping))]
    mapping[1.0]='float-literal';events.extend(['size after float literal='+str(len(mapping)),'get via int literal='+mapping[float(1)],'get via float literal='+mapping[1.0]])
    events.extend('key='+render(key) for key in mapping);wide={1:'one'};sf={float(2),2.0}
    events.extend(['wide size='+str(len(wide)),'wide get='+wide[1],'set size='+str(len(sf)),'set contains int literal='+render(float(2) in sf)])
    rows['map_key_contextual_literal']=events
    enum=lambda declared,variant,payload=():('module',declared,variant,payload)
    left=enum('E','A',(1,));values=[left==enum('E','A',(1,)),left==enum('E','A',(2,)),enum('E','B',('x',))==enum('E','B',('x',)),enum('E','B',('x',))==enum('E','B',('y',)),left==enum('E','C')]
    one,two=enum('K','Id',(1,)),enum('K','Id',(2,));mapping={one:'one',two:'two'};values.extend([len(mapping),mapping[one],mapping[two],len({one,two,one})])
    rows['enum_payload_equality']=values
    rows.update({name:trace['outputs'] for name,trace in enum_surface_traces().items()})
    if set(rows)!=_NAMES:raise ValueError('Aggregate model lost a complete original program')
    return rows

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):
        raise ValueError('Aggregate model lost exact source identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','src','stdlib'}:
            raise ValueError('Aggregate input is not a delivered repository identity')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Aggregate source/authority changed: '+name)
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        if source not in _INPUTS or Path(source).stem!=name:raise ValueError('Aggregate program lost its original source')
        rows.append({'id':'complete_aggregate_'+name,'source':source,'original_source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values[name]),'stderr_utf8':''},
            'model':'Complete original body; independent path borrow lifetime, exact object fields, tagged union, unsigned arithmetic, nominal enum payload and key equivalence models. No physical release or backend agreement oracle.',
            'observation_count':len(values[name])})
    return rows
