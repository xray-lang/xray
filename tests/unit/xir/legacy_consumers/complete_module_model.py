"""Independent complete module initialization, imported calls and value ownership traces."""
from pathlib import Path
import hashlib
import re

ROOT=Path(__file__).resolve().parent.parents[3]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '3aeabb14d154ef235fe51c665a7d367c2e51dc2c6f5261a9e0b194b228a9b945',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '4b07469dd5109d4c6d0b99896e72c735e60e1821f716352ffd0e4da4e75d563b',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/012-11-modules.md': '3c32493b1e22d29c459362a9f465a6f5316856b5b375c3d1c1e06025756eecd8',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '2e5cba69a0dc07fd91bbe6d6968dafb518f765296c4597e3541cc6d9de2b445b',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/math/math.xr': '0126a1eb1db1760b5950da3f6be0c47f504f65070afaff3757f7b45f27a86115',
 'stdlib/runtime/runtime.c': 'e969dd60094f379950636359499ad2951eca95062c89dcae1d29857dc0c08e4f',
 'stdlib/runtime/runtime.xr': '9f4d87664a727e7b29ceec161efe07d67d4e416c47030b06c682062f8f2fe57c',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/string.xr': 'b18d600b6b3627fcaa6c0801d8798de5fc02e0a45987da65d3e3fdbbd7c55132',
 'tests/diff/cases/semantics/modules/_animals.xr': '74fcfe2315b57f4c32c163f72741fcf9206900aea49c7bad1320bcd4d6e24c96',
 'tests/diff/cases/semantics/modules/_borrowed_arg_lib.xr': '45ac2aa9664bf5d894a40e8d24e98bf0c8a0df5dd4b271d4b9b9f24952767cbf',
 'tests/diff/cases/semantics/modules/_classlib.xr': 'fe1a944ced743e3500073526e0f1a4baa1ed5c4e38f87827a5639c1dc595f819',
 'tests/diff/cases/semantics/modules/_cleanup_init_error.xr': '2d92ba54d53f2c753195aaacde2465e8be4e11a47fd395db4fd47592ab5ac8d8',
 'tests/diff/cases/semantics/modules/_cleanup_init_panic.xr': '03742c9af3637ffc5837c338348382226bf35e88d57c8986dd43cdd91408b780',
 'tests/diff/cases/semantics/modules/_default_arg_class_lib.xr': '49809eb2321f7aadb5234ba56ab9ab4cb5b290782162a271e4dded3ea8acc9ee',
 'tests/diff/cases/semantics/modules/_exported_struct_methods_lib.xr': '726c2bf332ea1ff24af42694ba8a652873fdd3023aec6d91b3b50d270a174f08',
 'tests/diff/cases/semantics/modules/_genbox.xr': 'a920b9d261d1187ce75f4bf9ea719086906a9475bb28c9d1d316f08178d937fa',
 'tests/diff/cases/semantics/modules/_inherit_lib.xr': 'd28124fb83be7238233b3878203ea98f8c2582fc6c09d1e4768c063a85dafd0e',
 'tests/diff/cases/semantics/modules/_nested_class_field_lib.xr': 'e7945dde250091942b288e58e4ec0f1ba36abcf2fd16bc66bb4107028fa0662b',
 'tests/diff/cases/semantics/modules/_opts_a.xr': '156a51d75d0c717ae02c55f74c610a5dcee05bb4348ab79b2d5d2613f5132fcc',
 'tests/diff/cases/semantics/modules/_opts_b.xr': 'f3a1e8b9f1c31627ecd08a2173e47d5cb2d82ee4963b94f8b2dc338b30f11230',
 'tests/diff/cases/semantics/modules/_ptr_offset_const_lib.xr': '790909ef590b7c0b3c198712aafe077c335cedaed53ea5ec8d5056fe6973d702',
 'tests/diff/cases/semantics/modules/_same_name_method_a.xr': '0d3f0f38b2c6d04b89312c462496b3933fce6a7fe72a0a0a55040730f272572c',
 'tests/diff/cases/semantics/modules/_same_name_method_b.xr': '3cb02b75792715e9a7f6b25d26010f6309db1ab624d84c78335ed42f1a4a52ec',
 'tests/diff/cases/semantics/modules/_value_struct_arg_lib.xr': '40e8bb036f86d621f09c80d33a587d1c82740a070e098e7686da7181b3f4764b',
 'tests/diff/cases/semantics/modules/_value_struct_return_release_lib.xr': 'dd38cc00938ea85c8506ba7e546dbd307d2753d18347928ef8bb873392ef055b',
 'tests/diff/cases/semantics/modules/cleanup_init_error.xr': 'f3586513ccb56c97e5e8cfa89bdb61fb7d9dd1b9c61c2c3be27d72476ccb5eee',
 'tests/diff/cases/semantics/modules/cleanup_init_panic.xr': 'e4f9901628f8bbc9d35b487450f5c2cc233e12d6ef4da4e2e1bbb554c6d032de',
 'tests/diff/cases/semantics/modules/cross_module_borrowed_arg_release.xr': '00bce462dc7a655b78562801e2b5c841ffa37397c942a96cf24adace2c36963a',
 'tests/diff/cases/semantics/modules/exported_struct_methods.xr': 'd2915f34950c20af317e615cda27d1dc933e31b572c13001172ee45599b1b387',
 'tests/diff/cases/semantics/modules/native_module_scalar_call_target.xr': '002acde44ec05561eb415f7ab15f52e94053900ad87a8d9f95981bb5c965b5f9',
 'tests/diff/cases/semantics/modules/value_struct_arg_alias_import.xr': '043d43c9aa9562871f1a1d47bdb4be02c66d218b135e1697310c1ed5acbc24c3',
 'tests/diff/cases/semantics/modules/value_struct_arg_without_type_import.xr': 'f22f44406afb1cd2566bf35e2b2f05f0a194805cf5e8d0163d2b1e7dfea35ef4',
 'tests/diff/cases/semantics/modules/value_struct_copy_without_type_import.xr': '03b0d5e1616ffb9834bc70b83134db2a65619efbf0d4c09302c22ce061f98831',
 'tests/diff/cases/semantics/modules/value_struct_return_release.xr': '443fa26234cee6318870c85bd48309e1307c41071d2ba54939c1512560601376',
 'tests/diff/cases/semantics/modules/xmod_class.xr': '8252b34095ced7380cedc89166a93707c628cdbf621db0c51e9cc53dae9b09de',
 'tests/diff/cases/semantics/modules/xmod_default_arg_class.xr': '364c8a3c9727eb8236e6c659a978a69bef885aca9669bd71e2e9ccbd4e0d3d71',
 'tests/diff/cases/semantics/modules/xmod_inherit.xr': 'af36d96ba23db291a2822d1dfbc3df6ec328e07ced619079fdb7905f910f563c',
 'tests/diff/cases/semantics/modules/xmod_inherit_lib_subclass.xr': '1ee37aa0d0c8dfd0e1f5f02900b722675ac009043d60ae415e056bea8d967222',
 'tests/diff/cases/semantics/modules/xmod_namespace_generic.xr': 'c602e9e9131cceaeb910764704ae513f394c622dba5347b88c858e9b62eebe2e',
 'tests/diff/cases/semantics/modules/xmod_nested_class_field.xr': '96ba8d1422d8f6a5c3277ae013dfe0a3a557fabfec827fe98b634cc962fcd66b',
 'tests/diff/cases/semantics/modules/xmod_same_name_classes.xr': 'faf5a18fee6475ba5e0752bed4bc2591f6e0b6a37a2b043f6bb40d60ada330d0',
 'tests/diff/cases/semantics/modules/xmod_same_name_methods.xr': '565b6f2321493b06c0fdda7e6b99ad72c798ad370731bec1b9e1e15977923224',
 'tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/_classlib.xr': 'b92731cd5f776ee021d4f013336b18681e2469cec59be2eabe025d7c38c01657',
 'tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/root.xr': '8252b34095ced7380cedc89166a93707c628cdbf621db0c51e9cc53dae9b09de',
 'tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/_exported_struct_methods_lib.xr': '34f3404454ccc397a51a1a6a2137b6ab20f3951bbc8e61996f13c512cda41d58',
 'tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/root.xr': '718a91bb475cbb24c2c79b631f33c8efc076f1fa230352ea2923f6beaab86670'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/012-11-modules.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/016-15-standard-library.md',
 'stdlib/math/math.xr',
 'stdlib/runtime/runtime.c',
 'stdlib/runtime/runtime.xr',
 'stdlib/types/array.xr',
 'stdlib/types/string.xr',
 'tests/diff/cases/semantics/modules/_animals.xr',
 'tests/diff/cases/semantics/modules/_borrowed_arg_lib.xr',
 'tests/diff/cases/semantics/modules/_classlib.xr',
 'tests/diff/cases/semantics/modules/_cleanup_init_error.xr',
 'tests/diff/cases/semantics/modules/_cleanup_init_panic.xr',
 'tests/diff/cases/semantics/modules/_default_arg_class_lib.xr',
 'tests/diff/cases/semantics/modules/_exported_struct_methods_lib.xr',
 'tests/diff/cases/semantics/modules/_genbox.xr',
 'tests/diff/cases/semantics/modules/_inherit_lib.xr',
 'tests/diff/cases/semantics/modules/_nested_class_field_lib.xr',
 'tests/diff/cases/semantics/modules/_opts_a.xr',
 'tests/diff/cases/semantics/modules/_opts_b.xr',
 'tests/diff/cases/semantics/modules/_ptr_offset_const_lib.xr',
 'tests/diff/cases/semantics/modules/_same_name_method_a.xr',
 'tests/diff/cases/semantics/modules/_same_name_method_b.xr',
 'tests/diff/cases/semantics/modules/_value_struct_arg_lib.xr',
 'tests/diff/cases/semantics/modules/_value_struct_return_release_lib.xr',
 'tests/diff/cases/semantics/modules/cleanup_init_error.xr',
 'tests/diff/cases/semantics/modules/cleanup_init_panic.xr',
 'tests/diff/cases/semantics/modules/cross_module_borrowed_arg_release.xr',
 'tests/diff/cases/semantics/modules/exported_struct_methods.xr',
 'tests/diff/cases/semantics/modules/native_module_scalar_call_target.xr',
 'tests/diff/cases/semantics/modules/value_struct_arg_alias_import.xr',
 'tests/diff/cases/semantics/modules/value_struct_arg_without_type_import.xr',
 'tests/diff/cases/semantics/modules/value_struct_copy_without_type_import.xr',
 'tests/diff/cases/semantics/modules/value_struct_return_release.xr',
 'tests/diff/cases/semantics/modules/xmod_class.xr',
 'tests/diff/cases/semantics/modules/xmod_default_arg_class.xr',
 'tests/diff/cases/semantics/modules/xmod_inherit.xr',
 'tests/diff/cases/semantics/modules/xmod_inherit_lib_subclass.xr',
 'tests/diff/cases/semantics/modules/xmod_namespace_generic.xr',
 'tests/diff/cases/semantics/modules/xmod_nested_class_field.xr',
 'tests/diff/cases/semantics/modules/xmod_same_name_classes.xr',
 'tests/diff/cases/semantics/modules/xmod_same_name_methods.xr',
 'tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/_classlib.xr',
 'tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/root.xr',
 'tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/_exported_struct_methods_lib.xr',
 'tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/root.xr')
_PROGRAMS = {'_classlib': 'tests/diff/cases/semantics/modules/_classlib.xr',
 '_cleanup_init_error': 'tests/diff/cases/semantics/modules/_cleanup_init_error.xr',
 '_cleanup_init_panic': 'tests/diff/cases/semantics/modules/_cleanup_init_panic.xr',
 '_default_arg_class_lib': 'tests/diff/cases/semantics/modules/_default_arg_class_lib.xr',
 '_exported_struct_methods_lib': 'tests/diff/cases/semantics/modules/_exported_struct_methods_lib.xr',
 '_inherit_lib': 'tests/diff/cases/semantics/modules/_inherit_lib.xr',
 '_ptr_offset_const_lib': 'tests/diff/cases/semantics/modules/_ptr_offset_const_lib.xr',
 '_value_struct_arg_lib': 'tests/diff/cases/semantics/modules/_value_struct_arg_lib.xr',
 'cleanup_init_error': 'tests/diff/cases/semantics/modules/cleanup_init_error.xr',
 'cleanup_init_panic': 'tests/diff/cases/semantics/modules/cleanup_init_panic.xr',
 'cross_module_borrowed_arg_release': 'tests/diff/cases/semantics/modules/cross_module_borrowed_arg_release.xr',
 'exported_struct_methods': 'tests/diff/cases/semantics/modules/exported_struct_methods.xr',
 'native_module_scalar_call_target': 'tests/diff/cases/semantics/modules/native_module_scalar_call_target.xr',
 'value_struct_arg_alias_import': 'tests/diff/cases/semantics/modules/value_struct_arg_alias_import.xr',
 'value_struct_arg_without_type_import': 'tests/diff/cases/semantics/modules/value_struct_arg_without_type_import.xr',
 'value_struct_copy_without_type_import': 'tests/diff/cases/semantics/modules/value_struct_copy_without_type_import.xr',
 'value_struct_return_release': 'tests/diff/cases/semantics/modules/value_struct_return_release.xr',
 'xmod_class': 'tests/diff/cases/semantics/modules/xmod_class.xr',
 'xmod_default_arg_class': 'tests/diff/cases/semantics/modules/xmod_default_arg_class.xr',
 'xmod_inherit': 'tests/diff/cases/semantics/modules/xmod_inherit.xr',
 'xmod_inherit_lib_subclass': 'tests/diff/cases/semantics/modules/xmod_inherit_lib_subclass.xr',
 'xmod_namespace_generic': 'tests/diff/cases/semantics/modules/xmod_namespace_generic.xr',
 'xmod_nested_class_field': 'tests/diff/cases/semantics/modules/xmod_nested_class_field.xr',
 'xmod_same_name_classes': 'tests/diff/cases/semantics/modules/xmod_same_name_classes.xr',
 'xmod_same_name_methods': 'tests/diff/cases/semantics/modules/xmod_same_name_methods.xr'}
_PORTS = {'_classlib': 'tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/_classlib.xr',
 '_exported_struct_methods_lib': 'tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/_exported_struct_methods_lib.xr',
 'exported_struct_methods': 'tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/root.xr',
 'xmod_class': 'tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/root.xr'}
_COMPANIONS = {'_classlib': [],
 '_cleanup_init_error': [],
 '_cleanup_init_panic': [],
 '_default_arg_class_lib': [],
 '_exported_struct_methods_lib': [],
 '_inherit_lib': [],
 '_ptr_offset_const_lib': [],
 '_value_struct_arg_lib': [],
 'cleanup_init_error': ['tests/diff/cases/semantics/modules/_cleanup_init_error.xr'],
 'cleanup_init_panic': ['tests/diff/cases/semantics/modules/_cleanup_init_panic.xr'],
 'cross_module_borrowed_arg_release': ['tests/diff/cases/semantics/modules/_borrowed_arg_lib.xr'],
 'exported_struct_methods': ['tests/unit/xir/legacy_consumers/fixtures/module_struct_factory/_exported_struct_methods_lib.xr'],
 'native_module_scalar_call_target': [],
 'value_struct_arg_alias_import': ['tests/diff/cases/semantics/modules/_value_struct_arg_lib.xr'],
 'value_struct_arg_without_type_import': ['tests/diff/cases/semantics/modules/_value_struct_arg_lib.xr'],
 'value_struct_copy_without_type_import': ['tests/diff/cases/semantics/modules/_value_struct_arg_lib.xr'],
 'value_struct_return_release': ['tests/diff/cases/semantics/modules/_value_struct_return_release_lib.xr'],
 'xmod_class': ['tests/unit/xir/legacy_consumers/fixtures/module_class_receiver/_classlib.xr'],
 'xmod_default_arg_class': ['tests/diff/cases/semantics/modules/_default_arg_class_lib.xr'],
 'xmod_inherit': ['tests/diff/cases/semantics/modules/_animals.xr'],
 'xmod_inherit_lib_subclass': ['tests/diff/cases/semantics/modules/_inherit_lib.xr'],
 'xmod_namespace_generic': ['tests/diff/cases/semantics/modules/_genbox.xr'],
 'xmod_nested_class_field': ['tests/diff/cases/semantics/modules/_nested_class_field_lib.xr'],
 'xmod_same_name_classes': ['tests/diff/cases/semantics/modules/_opts_a.xr',
                            'tests/diff/cases/semantics/modules/_opts_b.xr'],
 'xmod_same_name_methods': ['tests/diff/cases/semantics/modules/_same_name_method_a.xr',
                            'tests/diff/cases/semantics/modules/_same_name_method_b.xr']}
_NAMES=frozenset({'_classlib','_cleanup_init_error','_cleanup_init_panic','_default_arg_class_lib',
    '_exported_struct_methods_lib','_inherit_lib','_ptr_offset_const_lib','_value_struct_arg_lib',
    'cleanup_init_error','cleanup_init_panic','cross_module_borrowed_arg_release','exported_struct_methods',
    'native_module_scalar_call_target','value_struct_arg_alias_import','value_struct_arg_without_type_import',
    'value_struct_copy_without_type_import','value_struct_return_release','xmod_class','xmod_default_arg_class',
    'xmod_inherit','xmod_inherit_lib_subclass','xmod_namespace_generic','xmod_nested_class_field',
    'xmod_same_name_classes','xmod_same_name_methods'})
_PORT_KEYS=frozenset({'_classlib','xmod_class','_exported_struct_methods_lib','exported_struct_methods'})


def observations():
    traces={name:[] for name in ['_classlib','_default_arg_class_lib','_exported_struct_methods_lib','_inherit_lib','_ptr_offset_const_lib','_value_struct_arg_lib']}
    for suffix,channel in [('error','error'),('panic','panic')]:
        events=[channel+' dependency body',channel+' dependency cleanup']
        traces['_cleanup_init_'+suffix]=events.copy();traces['cleanup_init_'+suffix]=events.copy()
    sum_lengths=lambda values:sum(len(value) for value in values)
    copied=bytearray([72,105])
    traces['cross_module_borrowed_arg_release']=[sum_lengths(['alpha','beta']),sum_lengths(['gamma','delta','epsilon']),sum([1,2,3,4,5]),sum([10,20,30]),bytes(copied).decode('utf-8'),'-'.join(['a','b','c'])]
    def state(seed):return {'data':[0]*4,'total':seed}
    def update(value,byte):value['data'][0]=byte;value['total']+=byte
    digest=lambda value:value['total']+value['data'][0]
    first=state(10);update(first,7)
    copied={'data':first['data'].copy(),'total':first['total']};update(copied,3)
    namespaced=state(20);update(namespaced,5)
    traces['exported_struct_methods']=[digest(first),digest(copied),digest(namespaced)]
    def pinned_random(low,high):
        if low!=high:raise ValueError('The original module scalar fixture requires equal random bounds')
        return low
    traces['native_module_scalar_call_target']=[pinned_random(7,7),pinned_random(3,3),pinned_random(4,4)+pinned_random(5,5)]
    make_pair=lambda value:{'a':value,'b':value*2}
    take_pair=lambda pair:pair['a']+pair['b']
    make_array=lambda value:[value,value*2,value*3]
    traces['value_struct_arg_alias_import']=[take_pair(make_pair(3))]
    traces['value_struct_arg_without_type_import']=[take_pair(make_pair(5)),take_pair(make_pair(7)),sum(make_array(2)),sum(make_array(4))]
    original=make_pair(1);copied=original.copy();copied['a']=99
    traces['value_struct_copy_without_type_import']=[original['a'],copied['a']]
    # The exact original counter predicates are required observations. Their
    # truth does not establish physical reclamation or allocator-fault coverage.
    checksum=0
    for index in range(10000):checksum+={'value':index}['value']
    traces['value_struct_return_release']=[checksum,True,True]
    counter={'value':10};values=[]
    for delta in [5,3]:counter['value']+=delta;values.append(counter['value'])
    other={'value':100}
    for delta in [1,40]:other['value']+=delta;values.append(other['value'])
    traces['xmod_class']=values
    def options(retries=2):return {'retries':retries,'label':'def'}
    def connect(port,opts=None):return port+(options() if opts is None else opts)['retries']
    def describe(opts=None):return (options() if opts is None else opts)['label']
    def bind(port=None):return 8000+25 if port is None else port
    def gateway(opts=None):return 9000+(options() if opts is None else opts)['retries']
    traces['xmod_default_arg_class']=[connect(7000),connect(7100),connect(7200,options()),connect(7300,options(9)),describe(),describe(options(4)),bind(),bind(1),gateway(),gateway(options(5)),bind()+1]
    animal={'name':'Rex','legs':4}
    traces['xmod_inherit']=['dog:'+animal['name'],animal['legs'],animal['name'],animal['legs']]
    sub={'b':1,'s':2};up=sub
    traces['xmod_inherit_lib_subclass']=[sub['b'],sub['s'],sub['s'],'sub',up['b']]
    box={'value':10};text={'value':'hi'}
    traces['xmod_namespace_generic']=[box['value'],(lambda value:value*2)(box['value']),text['value'],(lambda value:value+'!')(text['value'])]
    outer={'inner':{'value':42}};traces['xmod_nested_class_field']=[outer['inner']['value']]
    # Module identity is part of a declaration identity even when names match.
    factories={('a','Options'):lambda value=10:{'weight':value},('b','Options'):lambda value=20:{'weight':value}}
    traces['xmod_same_name_classes']=[factories['a','Options'](3)['weight'],factories['b','Options'](7)['weight'],factories['a','Options']()['weight'],factories['b','Options']()['weight']]
    methods={('a','Widget'):{'kind':100,'scale':1},('b','Widget'):{'kind':200,'scale':10}}
    traces['xmod_same_name_methods']=[methods['a','Widget']['kind'],methods['b','Widget']['kind'],3*methods['a','Widget']['scale'],3*methods['b','Widget']['scale']]
    if set(traces)!=_NAMES:raise ValueError('Complete module model lost a program')
    return traces


def render(value):
    if type(value) is bool:return str(value).lower()
    if type(value) in (int,str):return str(value)
    raise ValueError('Module observations require explicit scalar values')


def termination(name):
    if name in {'_cleanup_init_error','cleanup_init_error'}:return 1,'[Uncaught Error] InitError.Failed\n'
    if name in {'_cleanup_init_panic','cleanup_init_panic'}:return 1,'[Uncaught Panic] E0420: division by zero\n'
    return 0,''


def expected_rows():
    if (set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS))
            or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES)
            or set(_PORTS)!=_PORT_KEYS or set(_COMPANIONS)!=_NAMES):
        raise ValueError('Module model lost reviewed source, port or imported companion identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:
            raise ValueError('Module models require delivered source identities')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Module source or authority changed: '+name)
    for name in _NAMES:
        for source in [_PROGRAMS[name],_PORTS.get(name,_PROGRAMS[name]),*_COMPANIONS[name]]:
            if source not in _INPUTS:raise ValueError('A module source or companion lost its exact binding')
    for name,original in _PROGRAMS.items():
        selected=_PORTS.get(name,original)
        text=(ROOT/selected).read_bytes().decode('utf-8')
        names=re.findall(r'(?m)^\s*import (?:\{[^\n]+\} from )?"\./([^"]+)"',text)
        parent=selected.rsplit('/',1)[0]
        direct=list(dict.fromkeys(parent+'/'+item+('' if item.endswith('.xr') else '.xr') for item in names))
        if _COMPANIONS[name]!=direct:raise ValueError('Module model lost an exact literal imported companion')
    transforms={'_classlib':[('ref bump(','bump(')],'xmod_class':[],
        '_exported_struct_methods_lib':[('static init(','static make(')],'exported_struct_methods':[('.init(','.make(')]}
    for name,changes in transforms.items():
        text=(ROOT/_PROGRAMS[name]).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
        for before,after in changes:
            if before not in text:raise ValueError('Module port lost the reviewed original syntax')
            text=text.replace(before,after)
        if (ROOT/_PORTS[name]).read_bytes().replace(b'\r\n',b'\n')!=text.encode('utf-8'):
            raise ValueError('Module port changed more than its reviewed receiver/factory spelling')
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        exit_code,stderr=termination(name)
        rows.append({'id':'complete_module_'+name,'source':_PORTS.get(name,source),'original_source':source,'required_target':None,
            'expected':{'exit_code':exit_code,'stdout_utf8':''.join(render(value)+'\n' for value in values[name]),'stderr_utf8':stderr},
            'model':'Complete original module obligation and full companion sources; independent initialization/call/value-copy events. Declaration-only execution does not invoke exports. Counter predicates are not physical release proof.',
            'observation_count':len(values[name]),'companions':list(_COMPANIONS[name]),
            'required_runtime_counter_predicates':[{'name':'liveBytes-beforeBytes','comparison':'<','limit':4096},{'name':'liveObjects-beforeObjects','comparison':'<','limit':100}] if name=='value_struct_return_release' else []})
    return rows
