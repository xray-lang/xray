"""Independent complete original program traces with frozen source bindings."""
import hashlib
import json
import complete_numeric_model
import complete_text_range_model
import complete_ownership_model
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]

"""Derive expected observations before invoking the Xray product."""
def _reclassified_expected():
    out = {}
    values = [1, 2]; snapshot = values.copy()
    values[0] = 9; values.append(3)
    out['002_shared_bare_local_heap'] = [2, snapshot[0], len(snapshot), values[0], len(values)]
    values = [0]; snapshot = values.copy(); values.append(1)
    out['012_ref_rejects_duplicate_in_arg'] = [snapshot[0], len(values), values[1]]
    holder = {'items': []}; holder['items'].append(1)
    out['013_ref_rejects_in_field_projection'] = [len(holder['items']), holder['items'][0]]
    values = [1, 2]; saved = values.copy(); first_length = len(saved)
    values[0] = 9; values.append(3)
    out['036_ref_return_escape'] = [first_length, saved[0], len(saved), values[0], len(values)]
    out['037_ref_field_store_escape'] = [saved[0], len(saved), values[0], len(values)]
    outer = [saved.copy()]
    out['038_ref_array_push_escape'] = [len(outer), len(outer[0]), outer[0][0], values[0], len(values)]
    value = 1; snapshot = value; value = snapshot + 1
    out['149_in_rejects_ref_alias_arg'] = [value]
    buf = [1, 2, 3]; result = buf.copy()
    out['181_defer_cleanup_blocks_return'] = ['cleanup ' + str(len(buf)), len(result)]
    node = {'tag': 1}; alias = node; node['tag'] = 9
    out['188_ref_rejects_alias_root_arg'] = [alias['tag'], node['tag'], alias['tag']]
    box = {'n': 0}; box['n'] += 1
    out['189_ref_rejects_receiver_overlap'] = [box['n'], box['n']]
    def fail(): raise RuntimeError('Boom')
    cell = [fail]
    def run(): cell[0]()
    cell[0] = lambda: None; run()
    out['closure_capture_blocks_rebind'] = ['rebound']
    array = [0 for _ in range(2)]
    out['array_get_unchecked_requires_unsafe'] = [array[0], len(array)]
    array[0] = 1
    out['array_set_unchecked_requires_unsafe'] = [array[0], array[1], len(array)]
    assert len(out) == 13 and out['012_ref_rejects_duplicate_in_arg'] == [0,2,1]
    assert out['036_ref_return_escape'][1] != out['036_ref_return_escape'][3]
    return {key: {'observations':values, 'stdout_utf8':''.join(str(v)+'\n' for v in values)} for key,values in out.items()}


"""Independent full-program Array traces; never derived from product stdout."""
from functools import reduce

def _array_expected():
    traces={}
    chars=['b','中',chr(0x1f600)]
    out=[len(chars),'rune',chars[0],ord(chars[1]),ord(chars[2])]
    chars[0]='a';chars.append('z');out.append('|'.join(chars))
    chars[1:3]=['x','x'];out += [''.join(chars),chars.index('x'),-1]
    chars += ['q']*(6-len(chars));out += [len(chars),chars[4],chars[5]]
    chars.sort(key=ord);out += [''.join(chars),ord(chars[0])]
    made=['k']*3;out += [len(made),''.join(made)]
    from_string=list('A中'+chr(0x1f600));out += ['-'.join(from_string),'rune']
    traces['array_char_compact']=out
    values=[]
    for item in [1,2,3,4]:values.append(item)
    mapped=[x*3 for x in values];kept=[x for x in values if x%2==1]
    out=[len(mapped),mapped[0],mapped[3],len(kept),kept[0],kept[1]]
    total=reduce(lambda acc,x:acc*10+x,values,0)
    each_total=0
    for item in values:each_total += item
    out += [total,each_total,next((x for x in values if x>2),None),next((x for x in values if x>10),None)]
    out += [next((i for i,x in enumerate(values) if x==3),-1),next((i for i,x in enumerate(values) if x>10),-1)]
    out += [all(x>0 for x in values),all(x<4 for x in values),any(x==4 for x in values),any(x>10 for x in values)]
    chained=reduce(lambda acc,x:acc+x,(x+1 for x in values if x+1>2),0);out.append(chained)
    raw=[1,2,3];mapped=[x+10 for x in raw];kept=[x for x in raw if x>1]
    out += [len(mapped),mapped[0],mapped[2],len(kept),kept[0],kept[1]]
    traces['array_hof_shared_core']=out
    arrays=[[1,2,3,4],[True,False,True,False],['left','middle','right'],[10,20,30,40]]
    traces['array_reverse_shared_core']=[item for xs in arrays for item in reversed(xs)]
    shifted=[]
    for source in arrays:
        owned=source.copy();shifted.append(owned.pop(0));shifted.extend(owned)
    traces['array_shift_shared_core']=shifted
    render=lambda v:'null' if v is None else str(v).lower() if type(v) is bool else str(v)
    assert len(traces['array_hof_shared_core'])==23 and len(traces['array_char_compact'])==18
    assert traces['array_hof_shared_core'][6]==1234 and traces['array_hof_shared_core'][16]==12
    assert traces['array_char_compact'][12]=='aqqxxz'
    return {name:{'observations':values,'stdout_utf8':''.join(render(v)+'\n' for v in values),'exit_code':0,'stderr_utf8':''} for name,values in traces.items()}


_BINDING_PATHS = frozenset(['spec/source/sections/002-1-lexical-structure.md',
 'spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'src/base/xconstants.h',
 'src/shared/xr_numeric_core.h',
 'src/shared/xr_range_core.h',
 'src/shared/xr_string_core.h',
 'src/shared/xr_string_parse_core.h',
 'src/xir/xxir_source_enum_text.inc.c',
 'stdlib/codegen/codegen.xr',
 'stdlib/text/text.xr',
 'stdlib/types/array.xr',
 'stdlib/types/channel.xr',
 'stdlib/types/f64.xr',
 'stdlib/types/map.xr',
 'stdlib/types/panic_info.xr',
 'stdlib/types/rune.xr',
 'stdlib/types/string.xr',
 'tests/compile_errors/generics/007_codegen_intrinsic_type_argument.xr',
 'tests/compile_errors/generics/007_codegen_intrinsic_type_argument.xr.expected',
 'tests/compile_errors/ownership/002_shared_bare_local_heap.xr',
 'tests/compile_errors/ownership/002_shared_bare_local_heap.xr.expected',
 'tests/compile_errors/ownership/012_ref_rejects_duplicate_in_arg.xr',
 'tests/compile_errors/ownership/012_ref_rejects_duplicate_in_arg.xr.expected',
 'tests/compile_errors/ownership/013_ref_rejects_in_field_projection.xr',
 'tests/compile_errors/ownership/013_ref_rejects_in_field_projection.xr.expected',
 'tests/compile_errors/ownership/036_ref_return_escape.xr',
 'tests/compile_errors/ownership/036_ref_return_escape.xr.expected',
 'tests/compile_errors/ownership/037_ref_field_store_escape.xr',
 'tests/compile_errors/ownership/037_ref_field_store_escape.xr.expected',
 'tests/compile_errors/ownership/038_ref_array_push_escape.xr',
 'tests/compile_errors/ownership/038_ref_array_push_escape.xr.expected',
 'tests/compile_errors/ownership/149_in_rejects_ref_alias_arg.xr',
 'tests/compile_errors/ownership/149_in_rejects_ref_alias_arg.xr.expected',
 'tests/compile_errors/ownership/181_defer_cleanup_blocks_return.xr',
 'tests/compile_errors/ownership/181_defer_cleanup_blocks_return.xr.expected',
 'tests/compile_errors/ownership/188_ref_rejects_alias_root_arg.xr',
 'tests/compile_errors/ownership/188_ref_rejects_alias_root_arg.xr.expected',
 'tests/compile_errors/ownership/189_ref_rejects_receiver_overlap.xr',
 'tests/compile_errors/ownership/189_ref_rejects_receiver_overlap.xr.expected',
 'tests/compile_errors/ownership/closure_capture_blocks_rebind.xr',
 'tests/compile_errors/ownership/closure_capture_blocks_rebind.xr.expected',
 'tests/compile_errors/type/array_get_unchecked_requires_unsafe.xr',
 'tests/compile_errors/type/array_get_unchecked_requires_unsafe.xr.expected',
 'tests/compile_errors/type/array_set_unchecked_requires_unsafe.xr',
 'tests/compile_errors/type/array_set_unchecked_requires_unsafe.xr.expected',
 'tests/compile_errors/type/builtin_name_class_redeclared.xr',
 'tests/compile_errors/type/builtin_name_class_redeclared.xr.expected',
 'tests/compile_errors/type/builtin_name_enum_redeclared.xr',
 'tests/compile_errors/type/builtin_name_enum_redeclared.xr.expected',
 'tests/compile_errors/type/builtin_name_interface_redeclared.xr',
 'tests/compile_errors/type/builtin_name_interface_redeclared.xr.expected',
 'tests/compile_errors/type/slice_return_function_type_multi_source_rejected.xr',
 'tests/compile_errors/type/slice_return_function_type_multi_source_rejected.xr.expected',
 'tests/diff/cases/basic/arrays.xr',
 'tests/diff/cases/basic/range_precedence.xr',
 'tests/diff/cases/basic/ranges.xr',
 'tests/diff/cases/basic/strings.xr',
 'tests/diff/cases/semantics/collections/array_char_compact.xr',
 'tests/diff/cases/semantics/collections/array_hof_shared_core.xr',
 'tests/diff/cases/semantics/collections/array_reverse_shared_core.xr',
 'tests/diff/cases/semantics/collections/array_shift_shared_core.xr',
 'tests/diff/cases/semantics/int/wrapping_fixed_width.xr',
 'tests/diff/cases/semantics/int_wrap/exact_integer_bit_intrinsics.xr',
 'tests/diff/cases/semantics/int_wrap/generic_int_format_shared_core.xr',
 'tests/diff/cases/semantics/int_wrap/int_bits_methods.xr',
 'tests/diff/cases/semantics/int_wrap/int_overflow_predicates.xr',
 'tests/diff/cases/semantics/int_wrap/int_safe_methods.xr',
 'tests/diff/cases/semantics/int_wrap/numeric_method_shared_core.xr',
 'tests/diff/cases/semantics/int_wrap/stringbuilder_numeric_shared_core.xr',
 'tests/diff/cases/semantics/int_wrap/uint64_divmod_unsigned.xr',
 'tests/diff/cases/semantics/ownership/auto_borrow_class_alias_return_downgrade.xr',
 'tests/diff/cases/semantics/ownership/auto_borrow_class_readonly_param.xr',
 'tests/diff/cases/semantics/ownership/auto_borrow_heap_readonly_param.xr',
 'tests/diff/cases/semantics/ownership/converged_capability_positive.xr',
 'tests/diff/cases/semantics/ownership/copy_deep_independent.xr',
 'tests/diff/cases/semantics/ownership/copy_preserves_type.xr',
 'tests/diff/cases/semantics/ownership/in_alias_reassigned_copy_escape_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_closure_copy_capture_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_forward_function_param_copy_escape_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_function_param_copy_escape_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_function_param_method_copy_escape_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_go_copy_argument_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_method_wrapper_param_copy_escape_allowed.xr',
 'tests/diff/cases/semantics/ownership/in_readonly_user_method_names.xr',
 'tests/diff/cases/semantics/ownership/in_ref_heap_borrows.xr',
 'tests/diff/cases/semantics/ownership/move_into_go.xr',
 'tests/diff/cases/semantics/ownership/move_local_heap.xr',
 'tests/diff/cases/semantics/ownership/move_local_in_fn.xr',
 'tests/diff/cases/semantics/ownership/shared_class_copy_go.xr',
 'tests/diff/cases/semantics/ownership/shared_freeze_go.xr',
 'tests/diff/cases/semantics/string/interpolation_display_shapes.xr',
 'tests/diff/cases/semantics/string/multiconcat.xr',
 'tests/diff/cases/semantics/string/string_indexof_start_shared_core.xr',
 'tests/diff/cases/semantics/string/string_surface_declared_methods.xr',
 'tests/unit/xir/legacy_consumers/complete_numeric_model.py',
 'tests/unit/xir/legacy_consumers/complete_ownership_model.py',
 'tests/unit/xir/legacy_consumers/complete_text_range_model.py',
 'tests/unit/xir/legacy_consumers/fixtures/complete_numeric_text_targets/arrays.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_numeric_text_targets/string_indexof_start_shared_core.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_numeric_text_targets/stringbuilder_numeric_shared_core.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/auto_borrow_class_alias_return_downgrade.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/auto_borrow_class_readonly_param.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/converged_capability_positive.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/copy_deep_independent.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/copy_preserves_type.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_alias_reassigned_copy_escape_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_closure_copy_capture_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_forward_function_param_copy_escape_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_function_param_copy_escape_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_function_param_method_copy_escape_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_go_copy_argument_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_method_wrapper_param_copy_escape_allowed.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_readonly_user_method_names.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_ref_heap_borrows.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/move_into_go.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/move_local_heap.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/move_local_in_fn.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/shared_class_copy_go.xr',
 'tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/shared_freeze_go.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/002_shared_bare_local_heap.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/012_ref_rejects_duplicate_in_arg.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/013_ref_rejects_in_field_projection.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/036_ref_return_escape.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/037_ref_field_store_escape.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/038_ref_array_push_escape.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/149_in_rejects_ref_alias_arg.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/181_defer_cleanup_blocks_return.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/188_ref_rejects_alias_root_arg.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/189_ref_rejects_receiver_overlap.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/array_get_unchecked_requires_unsafe.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/array_set_unchecked_requires_unsafe.xr',
 'tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/closure_capture_blocks_rebind.xr',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py'])
_SOURCE_IDENTITIES = {'complete_original_text_range_ranges': ('tests/diff/cases/basic/ranges.xr', 'tests/diff/cases/basic/ranges.xr', 'text_range', 'ranges', True),
 'complete_original_text_range_range_precedence': ('tests/diff/cases/basic/range_precedence.xr', 'tests/diff/cases/basic/range_precedence.xr', 'text_range', 'range_precedence', True),
 'complete_original_array_array_char_compact': ('tests/diff/cases/semantics/collections/array_char_compact.xr',
                                                'tests/diff/cases/semantics/collections/array_char_compact.xr',
                                                'arrays',
                                                'array_char_compact',
                                                True),
 'complete_original_array_array_hof_shared_core': ('tests/diff/cases/semantics/collections/array_hof_shared_core.xr',
                                                   'tests/diff/cases/semantics/collections/array_hof_shared_core.xr',
                                                   'arrays',
                                                   'array_hof_shared_core',
                                                   True),
 'complete_original_array_array_reverse_shared_core': ('tests/diff/cases/semantics/collections/array_reverse_shared_core.xr',
                                                       'tests/diff/cases/semantics/collections/array_reverse_shared_core.xr',
                                                       'arrays',
                                                       'array_reverse_shared_core',
                                                       True),
 'complete_original_array_array_shift_shared_core': ('tests/diff/cases/semantics/collections/array_shift_shared_core.xr',
                                                     'tests/diff/cases/semantics/collections/array_shift_shared_core.xr',
                                                     'arrays',
                                                     'array_shift_shared_core',
                                                     True),
 'complete_original_frozen_positive_002_shared_bare_local_heap': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/002_shared_bare_local_heap.xr',
                                                                  'tests/compile_errors/ownership/002_shared_bare_local_heap.xr',
                                                                  'reclassified',
                                                                  '002_shared_bare_local_heap',
                                                                  False),
 'complete_original_frozen_positive_012_ref_rejects_duplicate_in_arg': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/012_ref_rejects_duplicate_in_arg.xr',
                                                                        'tests/compile_errors/ownership/012_ref_rejects_duplicate_in_arg.xr',
                                                                        'reclassified',
                                                                        '012_ref_rejects_duplicate_in_arg',
                                                                        False),
 'complete_original_frozen_positive_013_ref_rejects_in_field_projection': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/013_ref_rejects_in_field_projection.xr',
                                                                           'tests/compile_errors/ownership/013_ref_rejects_in_field_projection.xr',
                                                                           'reclassified',
                                                                           '013_ref_rejects_in_field_projection',
                                                                           False),
 'complete_original_frozen_positive_036_ref_return_escape': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/036_ref_return_escape.xr',
                                                             'tests/compile_errors/ownership/036_ref_return_escape.xr',
                                                             'reclassified',
                                                             '036_ref_return_escape',
                                                             False),
 'complete_original_frozen_positive_037_ref_field_store_escape': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/037_ref_field_store_escape.xr',
                                                                  'tests/compile_errors/ownership/037_ref_field_store_escape.xr',
                                                                  'reclassified',
                                                                  '037_ref_field_store_escape',
                                                                  False),
 'complete_original_frozen_positive_038_ref_array_push_escape': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/038_ref_array_push_escape.xr',
                                                                 'tests/compile_errors/ownership/038_ref_array_push_escape.xr',
                                                                 'reclassified',
                                                                 '038_ref_array_push_escape',
                                                                 False),
 'complete_original_frozen_positive_149_in_rejects_ref_alias_arg': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/149_in_rejects_ref_alias_arg.xr',
                                                                    'tests/compile_errors/ownership/149_in_rejects_ref_alias_arg.xr',
                                                                    'reclassified',
                                                                    '149_in_rejects_ref_alias_arg',
                                                                    False),
 'complete_original_frozen_positive_181_defer_cleanup_blocks_return': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/181_defer_cleanup_blocks_return.xr',
                                                                       'tests/compile_errors/ownership/181_defer_cleanup_blocks_return.xr',
                                                                       'reclassified',
                                                                       '181_defer_cleanup_blocks_return',
                                                                       False),
 'complete_original_frozen_positive_188_ref_rejects_alias_root_arg': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/188_ref_rejects_alias_root_arg.xr',
                                                                      'tests/compile_errors/ownership/188_ref_rejects_alias_root_arg.xr',
                                                                      'reclassified',
                                                                      '188_ref_rejects_alias_root_arg',
                                                                      False),
 'complete_original_frozen_positive_189_ref_rejects_receiver_overlap': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/189_ref_rejects_receiver_overlap.xr',
                                                                        'tests/compile_errors/ownership/189_ref_rejects_receiver_overlap.xr',
                                                                        'reclassified',
                                                                        '189_ref_rejects_receiver_overlap',
                                                                        False),
 'complete_original_frozen_positive_array_get_unchecked_requires_unsafe': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/array_get_unchecked_requires_unsafe.xr',
                                                                           'tests/compile_errors/type/array_get_unchecked_requires_unsafe.xr',
                                                                           'reclassified',
                                                                           'array_get_unchecked_requires_unsafe',
                                                                           False),
 'complete_original_frozen_positive_array_set_unchecked_requires_unsafe': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/array_set_unchecked_requires_unsafe.xr',
                                                                           'tests/compile_errors/type/array_set_unchecked_requires_unsafe.xr',
                                                                           'reclassified',
                                                                           'array_set_unchecked_requires_unsafe',
                                                                           False),
 'complete_original_frozen_positive_closure_capture_blocks_rebind': ('tests/unit/xir/legacy_consumers/fixtures/frozen_positive_rejections/closure_capture_blocks_rebind.xr',
                                                                     'tests/compile_errors/ownership/closure_capture_blocks_rebind.xr',
                                                                     'reclassified',
                                                                     'closure_capture_blocks_rebind',
                                                                     False),
 'complete_original_numeric_exact_integer_bit_intrinsics': ('tests/diff/cases/semantics/int_wrap/exact_integer_bit_intrinsics.xr',
                                                            'tests/diff/cases/semantics/int_wrap/exact_integer_bit_intrinsics.xr',
                                                            'numeric',
                                                            'exact_integer_bit_intrinsics',
                                                            True),
 'complete_original_numeric_int_bits_methods': ('tests/diff/cases/semantics/int_wrap/int_bits_methods.xr',
                                                'tests/diff/cases/semantics/int_wrap/int_bits_methods.xr',
                                                'numeric',
                                                'int_bits_methods',
                                                True),
 'complete_original_numeric_int_overflow_predicates': ('tests/diff/cases/semantics/int_wrap/int_overflow_predicates.xr',
                                                       'tests/diff/cases/semantics/int_wrap/int_overflow_predicates.xr',
                                                       'numeric',
                                                       'int_overflow_predicates',
                                                       True),
 'complete_original_numeric_int_safe_methods': ('tests/diff/cases/semantics/int_wrap/int_safe_methods.xr',
                                                'tests/diff/cases/semantics/int_wrap/int_safe_methods.xr',
                                                'numeric',
                                                'int_safe_methods',
                                                True),
 'complete_original_numeric_numeric_method_shared_core': ('tests/diff/cases/semantics/int_wrap/numeric_method_shared_core.xr',
                                                          'tests/diff/cases/semantics/int_wrap/numeric_method_shared_core.xr',
                                                          'numeric',
                                                          'numeric_method_shared_core',
                                                          True),
 'complete_original_numeric_uint64_divmod_unsigned': ('tests/diff/cases/semantics/int_wrap/uint64_divmod_unsigned.xr',
                                                      'tests/diff/cases/semantics/int_wrap/uint64_divmod_unsigned.xr',
                                                      'numeric',
                                                      'uint64_divmod_unsigned',
                                                      True),
 'complete_original_numeric_wrapping_fixed_width': ('tests/diff/cases/semantics/int/wrapping_fixed_width.xr',
                                                    'tests/diff/cases/semantics/int/wrapping_fixed_width.xr',
                                                    'numeric',
                                                    'wrapping_fixed_width',
                                                    True),
 'complete_original_ownership_auto_borrow_heap_readonly_param': ('tests/diff/cases/semantics/ownership/auto_borrow_heap_readonly_param.xr',
                                                                 'tests/diff/cases/semantics/ownership/auto_borrow_heap_readonly_param.xr',
                                                                 'ownership',
                                                                 'auto_borrow_heap_readonly_param',
                                                                 True),
 'complete_target_numeric_stringbuilder_numeric_shared_core': ('tests/unit/xir/legacy_consumers/fixtures/complete_numeric_text_targets/stringbuilder_numeric_shared_core.xr',
                                                               'tests/diff/cases/semantics/int_wrap/stringbuilder_numeric_shared_core.xr',
                                                               'numeric',
                                                               'stringbuilder_numeric_shared_core',
                                                               False),
 'complete_target_ownership_auto_borrow_class_alias_return_downgrade': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/auto_borrow_class_alias_return_downgrade.xr',
                                                                        'tests/diff/cases/semantics/ownership/auto_borrow_class_alias_return_downgrade.xr',
                                                                        'ownership',
                                                                        'auto_borrow_class_alias_return_downgrade',
                                                                        False),
 'complete_target_ownership_auto_borrow_class_readonly_param': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/auto_borrow_class_readonly_param.xr',
                                                                'tests/diff/cases/semantics/ownership/auto_borrow_class_readonly_param.xr',
                                                                'ownership',
                                                                'auto_borrow_class_readonly_param',
                                                                False),
 'complete_target_ownership_converged_capability_positive': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/converged_capability_positive.xr',
                                                             'tests/diff/cases/semantics/ownership/converged_capability_positive.xr',
                                                             'ownership',
                                                             'converged_capability_positive',
                                                             False),
 'complete_target_ownership_copy_deep_independent': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/copy_deep_independent.xr',
                                                     'tests/diff/cases/semantics/ownership/copy_deep_independent.xr',
                                                     'ownership',
                                                     'copy_deep_independent',
                                                     False),
 'complete_target_ownership_copy_preserves_type': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/copy_preserves_type.xr',
                                                   'tests/diff/cases/semantics/ownership/copy_preserves_type.xr',
                                                   'ownership',
                                                   'copy_preserves_type',
                                                   False),
 'complete_target_ownership_in_alias_reassigned_copy_escape_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_alias_reassigned_copy_escape_allowed.xr',
                                                                       'tests/diff/cases/semantics/ownership/in_alias_reassigned_copy_escape_allowed.xr',
                                                                       'ownership',
                                                                       'in_alias_reassigned_copy_escape_allowed',
                                                                       False),
 'complete_target_ownership_in_closure_copy_capture_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_closure_copy_capture_allowed.xr',
                                                               'tests/diff/cases/semantics/ownership/in_closure_copy_capture_allowed.xr',
                                                               'ownership',
                                                               'in_closure_copy_capture_allowed',
                                                               False),
 'complete_target_ownership_in_forward_function_param_copy_escape_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_forward_function_param_copy_escape_allowed.xr',
                                                                             'tests/diff/cases/semantics/ownership/in_forward_function_param_copy_escape_allowed.xr',
                                                                             'ownership',
                                                                             'in_forward_function_param_copy_escape_allowed',
                                                                             False),
 'complete_target_ownership_in_function_param_copy_escape_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_function_param_copy_escape_allowed.xr',
                                                                     'tests/diff/cases/semantics/ownership/in_function_param_copy_escape_allowed.xr',
                                                                     'ownership',
                                                                     'in_function_param_copy_escape_allowed',
                                                                     False),
 'complete_target_ownership_in_function_param_method_copy_escape_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_function_param_method_copy_escape_allowed.xr',
                                                                            'tests/diff/cases/semantics/ownership/in_function_param_method_copy_escape_allowed.xr',
                                                                            'ownership',
                                                                            'in_function_param_method_copy_escape_allowed',
                                                                            False),
 'complete_target_ownership_in_go_copy_argument_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_go_copy_argument_allowed.xr',
                                                           'tests/diff/cases/semantics/ownership/in_go_copy_argument_allowed.xr',
                                                           'ownership',
                                                           'in_go_copy_argument_allowed',
                                                           False),
 'complete_target_ownership_in_method_wrapper_param_copy_escape_allowed': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_method_wrapper_param_copy_escape_allowed.xr',
                                                                           'tests/diff/cases/semantics/ownership/in_method_wrapper_param_copy_escape_allowed.xr',
                                                                           'ownership',
                                                                           'in_method_wrapper_param_copy_escape_allowed',
                                                                           False),
 'complete_target_ownership_in_readonly_user_method_names': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_readonly_user_method_names.xr',
                                                             'tests/diff/cases/semantics/ownership/in_readonly_user_method_names.xr',
                                                             'ownership',
                                                             'in_readonly_user_method_names',
                                                             False),
 'complete_target_ownership_in_ref_heap_borrows': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/in_ref_heap_borrows.xr',
                                                   'tests/diff/cases/semantics/ownership/in_ref_heap_borrows.xr',
                                                   'ownership',
                                                   'in_ref_heap_borrows',
                                                   False),
 'complete_target_ownership_move_into_go': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/move_into_go.xr',
                                            'tests/diff/cases/semantics/ownership/move_into_go.xr',
                                            'ownership',
                                            'move_into_go',
                                            False),
 'complete_target_ownership_move_local_heap': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/move_local_heap.xr',
                                               'tests/diff/cases/semantics/ownership/move_local_heap.xr',
                                               'ownership',
                                               'move_local_heap',
                                               False),
 'complete_target_ownership_move_local_in_fn': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/move_local_in_fn.xr',
                                                'tests/diff/cases/semantics/ownership/move_local_in_fn.xr',
                                                'ownership',
                                                'move_local_in_fn',
                                                False),
 'complete_target_ownership_shared_class_copy_go': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/shared_class_copy_go.xr',
                                                    'tests/diff/cases/semantics/ownership/shared_class_copy_go.xr',
                                                    'ownership',
                                                    'shared_class_copy_go',
                                                    False),
 'complete_target_ownership_shared_freeze_go': ('tests/unit/xir/legacy_consumers/fixtures/complete_ownership_targets/shared_freeze_go.xr',
                                                'tests/diff/cases/semantics/ownership/shared_freeze_go.xr',
                                                'ownership',
                                                'shared_freeze_go',
                                                False),
 'complete_target_text_range_arrays': ('tests/unit/xir/legacy_consumers/fixtures/complete_numeric_text_targets/arrays.xr',
                                       'tests/diff/cases/basic/arrays.xr',
                                       'text_range',
                                       'arrays',
                                       False),
 'complete_target_text_range_string_indexof_start_shared_core': ('tests/unit/xir/legacy_consumers/fixtures/complete_numeric_text_targets/string_indexof_start_shared_core.xr',
                                                                 'tests/diff/cases/semantics/string/string_indexof_start_shared_core.xr',
                                                                 'text_range',
                                                                 'string_indexof_start_shared_core',
                                                                 False),
 'complete_original_text_range_multiconcat': ('tests/diff/cases/semantics/string/multiconcat.xr', 'tests/diff/cases/semantics/string/multiconcat.xr', 'text_range', 'multiconcat', True),
 'complete_original_text_range_strings': ('tests/diff/cases/basic/strings.xr', 'tests/diff/cases/basic/strings.xr', 'text_range', 'strings', True),
 'complete_original_text_range_interpolation_display_shapes': ('tests/diff/cases/semantics/string/interpolation_display_shapes.xr', 'tests/diff/cases/semantics/string/interpolation_display_shapes.xr', 'text_range', 'interpolation_display_shapes', True)}

def require_owned_input_paths(paths):
    for name in paths:
        if not isinstance(name,str) or not name or ':' in name or '\\' in name:
            raise ValueError('model inputs require canonical repository-relative source identities')
        path = PurePosixPath(name)
        if (path.is_absolute() or path.as_posix() != name or '..' in path.parts
                or path.parts[0] not in {'tests','src','stdlib','spec'}
                or any(part.startswith('.tmp') for part in path.parts)):
            raise ValueError('model inputs must belong to delivered source roots')


def expected_rows():
    bindings = json.loads((HERE / 'complete_program_bindings.json').read_text(encoding='utf-8'))
    input_paths = [item['path'] for item in bindings['inputs']]
    require_owned_input_paths(input_paths)
    identities = {entry['id']: (entry['source'], entry['original_source'], entry['family'],
                               entry['model_key'], entry['original_run_corpus'])
                  for entry in bindings['entries']}
    if (set(input_paths) != _BINDING_PATHS or len(input_paths) != len(_BINDING_PATHS)
            or identities != _SOURCE_IDENTITIES or len(identities) != len(bindings['entries'])):
        raise ValueError('complete original model changed a source identity or required binding')
    input_hashes = {item['path']: item['lf_sha256'] for item in bindings['inputs']}
    for entry in bindings['entries']:
        if (input_hashes[entry['source']] != entry['source_lf_sha256']
                or input_hashes[entry['original_source']] != entry['original_lf_sha256']):
            raise ValueError('complete original source metadata disagrees with its binding')
    for item in bindings['inputs']:
        data = (ROOT / item['path']).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != item['lf_sha256']:
            raise ValueError('complete original source or semantic authority changed: ' + item['path'])
    derived = {'reclassified': _reclassified_expected(), 'arrays': _array_expected()}
    derived['ownership'] = {row['id']: row for row in complete_ownership_model.expected()}
    derived['numeric'] = {row['id']: row for row in complete_numeric_model.expected()
                          if row['id'] in ['exact_integer_bit_intrinsics', 'int_bits_methods', 'int_overflow_predicates', 'int_safe_methods', 'numeric_method_shared_core', 'stringbuilder_numeric_shared_core', 'uint64_divmod_unsigned', 'wrapping_fixed_width']}
    derived['text_range'] = {row['id']: row for row in complete_text_range_model.expected()
                             if row['id'] in ['arrays', 'string_indexof_start_shared_core', 'range_precedence', 'ranges', 'multiconcat', 'strings', 'interpolation_display_shapes']}
    rows = []
    identifiers = set()
    for entry in bindings['entries']:
        if entry['id'] in identifiers:
            raise ValueError('duplicate complete original model identity')
        identifiers.add(entry['id'])
        model = derived[entry['family']][entry['model_key']]
        if entry['family'] == 'reclassified':
            original = (ROOT / entry['original_source']).read_bytes().replace(b'\r\n', b'\n')
            selected = (ROOT / entry['source']).read_bytes().replace(b'\r\n', b'\n')
            if selected != original + entry['observation_suffix'].encode('utf-8'):
                raise ValueError('complete original body or observation suffix changed')
        rows.append({'id': entry['id'], 'source': entry['source'],
                     'original_source': entry['original_source'],
                     'original_run_corpus': entry['original_run_corpus'],
                     'required_target': 'windows-x86_64' if entry['family'] == 'numeric' else None,
                     'expected': {'exit_code': 0, 'stdout_utf8': model['stdout_utf8'], 'stderr_utf8': ''},
                     'model': 'Independent complete original trace under frozen value, identity, call and Array rules.',
                     'observation_count': len(model['observations'])})
    expected_keys = {(family, key) for family, values in derived.items() for key in values}
    actual_keys = {(entry['family'], entry['model_key']) for entry in bindings['entries']}
    if actual_keys != expected_keys or len(actual_keys) != len(bindings['entries']):
        raise ValueError('complete original model lost a program or duplicated an alias')
    return rows


def original_expected_rows():
    return [row for row in expected_rows() if row['original_run_corpus']]


def require_target(model, target):
    required = model.get('required_target')
    if required is not None and target != required:
        raise ValueError('complete numeric model requires the declared target ABI: ' + required)
