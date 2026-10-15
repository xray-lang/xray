"""Derive original execution traces without reading Xray VM or native output."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def f32(value: float) -> float:
    return struct.unpack('<f', struct.pack('<f', value))[0]


def render_f32(value: float) -> str:
    for digits in range(1, 10):
        text = format(value, f'.{digits}g')
        if struct.pack('<f', float(text)) == struct.pack('<f', value):
            return text if '.' in text or 'e' in text else text + '.0'
    raise ValueError('binary32 model has no round-trip decimal')


def observations() -> dict[str, tuple[list, str]]:
    a, b, c, d = .1, .2, .3, .4
    x, big, small = 1.7, 123456789.123456789, .000123456789
    af, bf = f32(.1), f32(.2)
    floats32 = [f32(af * bf), f32(af + bf), f32(f32(af * af) + f32(bf * bf)), f32(1.0 / 3.0)]
    high, all_bits = 1 << 63, (1 << 64) - 1
    block = ['plain-start', 'plain-body', 'plain-defer', 'plain-after',
             'return-inner', 'return-root', 'ret', '--', 'return-fall', 'return-inner',
             'return-after', 'return-root', 'end', '--']
    for i in range(2):
        block += [f'break-body {i}' if i == 0 else 'break-now', f'break-defer {i}']
    block += ['after-break']
    for i in range(3):
        block += [f'continue-body {i}' if i != 1 else 'continue-now', f'continue-defer {i}']
    block += ['after-continue', '--', 'value-try', 'value-caught', 'value-after', 'value-root',
              '--', 'panic-try', 'panic-caught', 'panic-after', 'panic-root', 'done']
    models = {
        'ffi/c_export_scalar': ([19 + 23], 'The ordinary source entry adds two i32 values; a C-export ABI consumer is a separate obligation.'),
        'cleanup/defer_block_scope_exits': (block, 'Nearest-block LIFO at fallthrough, both return paths, break, continue, enum error and caught panic; unreachable writes are absent.'),
        'cleanup/defer_builtin_method_mix': (['body', 'block', 'drop B', 'fn free', 'builtin', 'done'], 'Four registered cleanup actions execute in reverse order after body.'),
        'cleanup/defer_coroutine': (['task-body 1', 'task-defer 1', 10, 'task-body 2', 'task-defer 2', 20, 'done'], 'Await completes the first task and its cleanup before the second go is issued.'),
        'cleanup/defer_panic_unwind': (['boom-defer', 'mid-defer-2', 'mid-defer-1', 'caught', 'top-defer', 'done'], 'Explicit panic catch unwinds child frames and LIFO cleanup; it is distinct from enum error catch.'),
        'float_fmt/f32_rounding': ([render_f32(v) for v in floats32], 'Round every binary32 operation independently, then choose a shortest round-trip decimal at binary32 precision.'),
        'float_fmt/finite': ([1.5, -1.5, 0.0, 100.0, .1, 3.141592653589793, 2.0 / 4.0, 123456789.0, .000125, -42.75], 'Binary64 literals and division; finite decimal formatting retains integral .0.'),
        'float_fmt/fma_contraction': ([a * b + c * d, a * b - c * d, a * a + b * b + c * c + d * d,
                                       ((2.5 * x + 1.25) * x + .75) * x + 3.125,
                                       big * small + a * b, big * a - small * d], 'Each CPython binary64 operation materializes before the next; no fused multiply-add or reassociation is used.'),
        'float_fmt/neg_zero_const_pool': ([-42.75 * 0.0, -0.0, 0.0, -0.0 * 1.0], 'Independent signed-zero multiplication and literal signs; equal numeric zeros retain distinct formatted signs.'),
        'float_fmt/special_values': ([-0.0, 0.0, 0.0 == -0.0, 0.0 * -1.0, -1.0 * 0.0, (0.0 * -1.0) == 0.0, 1.0 / 2.0, -1.0 / 4.0], 'IEEE signed zeros compare equal while output retains their sign; exact powers-of-two division.'),
        'int_wrap/uint64_print_compare': ([high, all_bits, str(high), str(high), f'concat:{high}:{all_bits}', f'tmpl:{high}:{all_bits}',
                                           high > 0, 0 < high, all_bits > high, high <= all_bits, all_bits >= high, high < 0, high > 1,
                                           high > 0, 0 < high, all_bits > high, 'branch-gt', 'branch-le'], 'Full unsigned decimal payload and order, including contextual literal operands and both branch paths.'),
        'modules/xmod_coro': ([6 * 6 + 1 + 9 * 9 + 1], 'Two imported square-plus results sum to 119; source output alone does not grant separate-object ABI or scheduling qualification.'),
        'modules/xmod_private_const_defaults': ([192, 17, 4 * (192 // 64), 5 * (192 // 64)], 'Private default constant resolves in the declaration module; explicit argument and both import spellings retain their values.'),
        'modules/xmod_static': ([1 * 1, 5 * 5, 3 * 3], 'Imported factory and constructor results retain their side fields for the instance method.'),
        'modules/xmod_static_defaults': (['named:tail', 'named:full', 5 + 7, 'ns:tail', 1 + 2, True, False], 'String, integer and nullable defaults resolve through selective and namespace imports.'),
        'ownership/auto_borrow_value_struct_return_downgrade': ([9, 2, 1, 2], 'Returned Pair is a logical value copy; changing q.a does not change p.'),
        'ownership/closure_mutable_scalar_direct_call': ([2 * 10 + (2 + 3)], 'Both closure calls mutate the same captured scalar, producing a=2 and b=5.'),
        'ownership/in_mutator_scalar_from_borrow': ([len([1, 2, 3, 4, 5])], 'Reading the input Array publishes only its scalar length into the explicit ref output.'),
        'ownership/in_return_scalar_from_borrow': ([len([1, 2, 3])], 'Returning a scalar length does not escape an Array view or consume the input value.'),
        'ownership/in_store_scalar_from_borrow': ([len([1, 2, 3, 4])], 'The explicit ref Counter receives the input length; no Array alias is stored.'),
        'ownership/loop_ref_array_pop_backedge': ([7 + 7], 'The four-op interpreter pushes twice, pops twice and returns 14; original chain assertion and all loop/backedge bodies remain.'),
        'ownership/shared_binding_boundaries': ([len([1, 2]), [3, 4][1]], 'Read-only logical values preserve length and indexed scalar at both binding boundaries.'),
        'ownership/stack_closure_cell_capture': ([1 * 10 + 2], 'Two nested closures append in source call order to the same captured Array value.'),
        'ownership/value_struct_alias_copy_param': ([(1 + 10) + 2, 1], 'Parameter and local Pair copies permit local mutation while the original field stays one.'),
        'condition/bool_nullable_explicit': (['yes', 'null-flag', 'coalesced-false'], 'Some(true), explicit null test and null-coalesced false choose three fixed branch observations.'),
        'exception/assertion_same_t_parity': (['assertion-same-t-parity-ok'], 'Preserve the complete contextual assertion program; this original banner alone leaves its internal scalar observations open.'),
        'observed/assertion_same_t_parity': (['assertion-same-t-parity-ok', 1 * 10 + 2, 1, 7, True, 7],
                                           'Keep the original source byte-for-byte, then observe actual/expected single-evaluation order, anchored call count, u8 payload, absent Optional and i64 value.'),
        'exception/catch_panic': (['ok:5', 'caught', 'outer-caught', 'done'], 'Integer division succeeds for two and the explicit panic channel catches zero divisors, including the nested invocation.'),
        'exception/match_non_exhaustive_panic_shape': (['non-exhaustive match', 442], 'Value two has no matching arm. The original panic catch prints the canonical match-failure message and public E0442 code; it never prints a successful result.'),
        'exception/defer_stack_growth': ([200, 42, 'ok'], 'The complete depth-200 recursive cleanup executes before the function result is printed; no recursion bound is reduced.'),
        'exception/pattern_catch': (['boom', 'other', 'payload:7', 'ok', 'any'], 'Five calls cover two bare variants, payload extraction, normal return and typed enum catch-all.'),
        'collections/array_unshift_shared_core': ([1, 2, 3, 4, True, False, True, False, 'left', 'middle', 'right', 10, 20, 30, 40], 'Prepending one value preserves complete old sequence order for integer, bool, string and u8 domains.'),
        'collections/byte_array_bool_short_circuit_liveness_shared_core': (['unmasked', 'masked'], 'The second octet has bit128 clear or set, matching the explicit nullable expectation without a mismatch write.'),
        'value_struct_generic_annotation': ([41, 99, 41, 7, 7, 100], 'Explicit generic values agree through variable, parameter, return and nested field; mutation of h2 does not alter h.'),
        'oop/enum_static_methods': (['Green', 'Blue', 'Green', 200], 'Named enum identity survives static factory, instance method, forward use and chained call.'),
        'oop/typename_class_instance': (['Point'], 'The constructed nominal root class keeps its source name; fields and constructor remain checked.'),
        'stdlib/time_query_system_direct': ([True] * 9, 'Current Windows epoch is positive; monotonic counters are nonnegative and reported minute offsets remain within a full day. Exact provider and external-clock qualification remain separate.'),
        'side_effects/arg_order': (['A', 'B', 'C', 1 + 2 + 3, 'X', 'Y', 10 + 20], 'Arguments and array literal elements evaluate once from left to right before their enclosing result.'),
        'side_effects/short_circuit': (['A', 'else1', 'C', 'then2'], 'False left-and and true left-or suppress B and D and choose the respective branches.'),
    }
    checksum = sum(6 * i + 3 for i in range(100))
    models['coro/cycle_leak_bounded_by_coroutine_heap'] = (
        [checksum, checksum, True],
        'Two complete 100-round tasks traverse six alternating tags per cycle. Repeated checksum does not prove physical cycle reclamation.')
    models['regression/0398_null_test_owner_edges'] = ([41, 7], 'Absent Optional chooses 41; present seven survives return and output.')
    models['match/adt_enum_nonpayload_variant'] = (
        ['circle:3', 'square:4', 'point', 'unknown', 9, -1, -2, -2, True],
        'All four semantic variants, wildcard results and equal bare variant descriptions are observed without guessing any runtime ordinal.')
    loop_cleanup = []
    for i in range(4):
        loop_cleanup.extend(['iter ' + str(i), 'blk ' + str(i)])
    loop_cleanup.append('loop-block-end')
    for i in range(3):
        loop_cleanup.extend(['call-iter ' + str(i), 'call ' + str(i)])
    loop_cleanup.extend(['loop-call-end', 'done'])
    models['cleanup/defer_loop_scope'] = (
        loop_cleanup,
        'Each complete original loop iteration owns its nearest block cleanup. '
        'The current iteration prints before its deferred block or call, before the next iteration begins.')
    user = {'name': 'Bob', 'age': 25}
    user_before = user['name']
    user['name'] = 'Ada'
    models['object/object_static_index_get_set'] = (
        [user_before, user['name']],
        'The constant name index reads Bob, updates that same named field once, then member access reads Ada.')
    xs = [1, 2]
    ys = xs.copy()
    xs.append(3)
    models['ownership/shared_copy_to_shared'] = (
        [len(xs), len(ys), ys[0]],
        'The original copy intent retains both elements in an independent logical Array value; '
        'appending to the mutable source changes only its length. The original public copy spelling '
        'still needs target-syntax admission and is never reclassified as an expected rejection.')
    models['observed/array_logical_const_copy'] = (
        [len(xs), len(ys), ys[0]],
        'The complete original Array copy intent uses the admitted ordinary const binding instead of '
        'the unresolved public copy spelling. Both original elements, the append and every output '
        'remain; this replacement does not qualify the untouched original copy spelling.')
    acc = [11, 12, 13, 14]
    acc_sum = sum(acc)
    acc[2] = 99
    models['int_wrap/fixed_array_uint64_lane'] = (
        [acc_sum, acc[2], acc[1]],
        'All four original u64 lanes contribute to50 before the third lane is replaced by99; '
        'the original variable index1 then reads the unchanged12.')
    text = 'hello'
    view_length = len(text.encode('utf-8'))
    text = 'bye'
    models['slice/string_byte_slice_receiver_nll'] = (
        [view_length, len(text)],
        'The complete original byte view has five UTF-8 bytes and its last use ends before '
        'the owner binding changes to the three-scalar bye string; no view is used after that change.')
    for helper in ('animals', 'borrowed_arg_lib', 'corolib', 'genbox', 'nested_class_field_lib',
                   'opts_a', 'opts_b', 'private_const_default', 'same_name_method_a',
                   'same_name_method_b', 'shapes', 'static_defaults',
                   'value_struct_return_release_lib', 'array_fill_ref_lib'):
        models['helper/' + helper] = (
            [],
            'The complete original module only declares helpers; its standalone run has no calls or output. '
            'The sole module-level DEFAULT_LIMIT in its helper is the pure literal192. '
            'No exported helper body, importer, inheritance, generic, coroutine, borrow or resource qualification is granted.')
    view_owner = [7]
    first_view_length, first_view_value = len(view_owner), view_owner[0]
    view_owner.append(9)
    models['collections/span_active_borrow_scope_shared_core'] = (
        [first_view_length, first_view_value, len(view_owner), view_owner[1]],
        'The complete original byte view is last read before the owner appends9. '
        'Both original byte values and both lengths remain; no post-append view read is added.')
    byte_owner = 'ABCD'.encode('utf-8')
    byte_window = byte_owner[1:3]
    byte_snapshot = list(byte_window)
    models['slice/string_byte_slice_view'] = (
        [len(byte_owner), byte_owner[0], byte_owner[1], len(byte_window),
         byte_window[0], byte_window[1], byte_snapshot[0]],
        'The complete original ASCII byte owner, two-byte window and owned copy yield '
        'all seven original outputs; UTF-8 byte values are computed independently.')
    apply = lambda fn, value: fn(value)
    apply_twice = lambda fn, value: fn(fn(value))
    mapped = [apply(lambda value: value * value, value) for value in [1, 2, 3]]
    cell = 10
    mapped_cell = apply(lambda value: value + 100, cell)
    cell = apply(lambda value: value * 2, cell)
    models['functions/higher_order_closures'] = (
        [apply(lambda value: value * 3, 10), apply_twice(lambda value: value + 1, 5),
         apply(lambda value: 'n=' + str(value), 7)] + mapped + [mapped_cell, cell, cell],
        'All original free higher-order calls, the complete mapInt loop, generic Cell map, '
        'ref update and final receiver field retain every original call and all nine outputs.')
    sum_ints = lambda *values: sum(values)
    sum_floats = lambda *values: sum(values, 0.0)
    count_true = lambda *values: sum(bool(value) for value in values)
    join_strings = lambda sep, *values: sep.join(values)
    wrap_sum = lambda base, left, right: base + sum_ints(left, right, base)
    models['functions/vararg_functions'] = (
        [sum_ints(), sum_ints(7), sum_ints(1, 2, 3, 4, 5), sum_floats(),
         sum_floats(1.5, 2.5, 3.0), count_true(True, False, True, True, False),
         join_strings('-'), join_strings('-', 'only'), join_strings(', ', 'a', 'b', 'c'),
         wrap_sum(10, 1, 2)],
        'The complete original zero/one/many argument, f64, bool and string loops and '
        'nested forwarding produce all ten outputs, including the empty join line and '
        'canonical0.0/7.0. Rest admission failures remain positive migration gaps.')
    first_match = lambda value: 'int: ' + str(value) if type(value) is int else 'a string'
    second_match = lambda value: ('i' if type(value) is int else 's') + str(value)
    models['condition/match_is_arms_no_separator'] = (
        [first_match(42), first_match('hello'), second_match(7), second_match('x')],
        'Both complete original consecutive type-pattern matches select their i64/string '
        'arms for all four calls, preserving interpolation and the exact spaces.')
    pair = {'first': 11, 'second': 31}
    words = ['x', 'ray']; box = {'value': 3.5}
    apply = lambda function, value: function(value)
    models['generic_type_alias'] = (
        [pair['first'] + pair['second'], words[0] + words[1],
         apply(lambda value: 'n=' + str(value), 7), box['value']],
        'The complete original expands structural Pair, Array Vec, callable Mapper and '
        'forward structural Box aliases; exact fields, element concatenation and the '
        'actual lambda argument produce all four values without executing Xray.')
    models['oop/override_checked_valid'] = (
        ['auto-override-ok'],
        'The complete original declares a matching override signature and prints its '
        'marker once. Neither speak body is called; this is complete declaration admission, '
        'not runtime dispatch or inheritance qualification.')
    return models


def expected_rows() -> list[dict]:
    binding = json.loads((HERE / 'independent_behavior_bindings.json').read_text(encoding='utf-8'))
    for entry in binding['inputs']:
        data = (ROOT / entry['path']).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(data).hexdigest() != entry['lf_sha256']:
            raise ValueError('independent behavior source or authority changed: ' + entry['path'])
    sources = binding['sources']
    models = observations()
    if set(models) != set(sources):
        raise ValueError('independent model lost a whole original source')
    result = []
    for name, (values, reason) in models.items():
        stdout = ''.join((str(v).lower() if type(v) is bool else str(v)) + '\n' for v in values)
        result.append({'id': 'independent_behavior_' + name.replace('/', '_'), 'source': sources[name],
                       'expected': {'exit_code': 0, 'stdout_utf8': stdout, 'stderr_utf8': ''},
                       'model': reason, 'observation_count': len(values)})
    return result


def named_expected_rows() -> list[dict]:
    # Thirty wide locals, sixteen middle locals and ten worker locals remain
    # live in the original source. Their sums give 56*seed+1464 independently.
    worker = lambda seed: sum(seed + k for k in range(10)) + (
        sum(seed + 9 + k for k in range(16)) + sum(seed + 24 + k for k in range(30)))
    first = [worker(0), worker(100), worker(200)]
    cases = {
        '1149_module_call_stack_grow': {
            'test_invoke_grows_combined_slab': first + [first[1] - first[0], first[2] - first[1]],
            'test_invoke_main_coro': [worker(0)],
            'test_repeated_go_reuses_slab': [worker(i) for i in range(32)],
        },
        '1151_inject_queue_spill': {'test_inject_queue_spill_chain': [2048]},
    }
    rows = []
    for name, models in cases.items():
        stdout = ''.join(str(value) + '\n' for values in models.values() for value in values)
        rows.append({'id': 'independent_behavior_named_' + name,
                     'expected': {'exit_code': 0, 'stdout_utf8': stdout, 'stderr_utf8': '',
                                  'required_test_names': list(models),
                                  'per_test_observations': {name: {'values': values} for name, values in models.items()}},
                     'model': 'Independent local-range sums and complete depth-2048 go/await recurrence; all original named bodies and 32 repeated tasks remain.'})
    return rows
