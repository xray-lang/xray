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
        'exception/catch_panic': (['ok:5', 'caught', 'outer-caught', 'done'], 'Integer division succeeds for two and the explicit panic channel catches zero divisors, including the nested invocation.'),
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
