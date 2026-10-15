"""Independent complete Channel bodies and their separate standalone route."""
import hashlib
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'tests/regression/11_coroutine/1155_channel_close_timeout_fanout.xr': 'c15c09543cd1fb97e2cdc35bc36e2ba73a3f44f3e3ec14d7ad8c24977a4c1271',
 'tests/regression/11_coroutine/1166_channel_map_deep_copy_stress.xr': '18380493799fc52aac015153c997f5dee397092e925cb9fab17ff18b10de3444',
 'tests/regression/11_coroutine/1167_channel_explicit_transfer.xr': 'cb4717b562b6a32c87949a110840374e738fbeae700089e67f2e610416acfd03',
 'tests/unit/xir/legacy_consumers/fixtures/channel_array_logical_values/root.xr': '63f7a4ae316538c30cb49b4ac4874726c7e24df32c057e9dde994e2684decaec',
 'tests/unit/xir/legacy_consumers/fixtures/channel_close_distinct_from_timeout/root.xr': 'd4f4cc3938f5fc876bf352eec4d854549fbd08bdf6f681724b36219438303f46',
 'tests/unit/xir/legacy_consumers/fixtures/channel_map_value_copy_stress/root.xr': 'ad40dcedf6b987e6341c93cc5dd7c078a3676e7d86830b0ec1de56614d74b4b2',
 'tests/unit/xir/legacy_consumers/fixtures/complete_channel_observations/arrays/root.xr': 'cc1be81d0a7acbe19a3912e9f9869a2d08755d3c7226cf0dc98433bbdec98295',
 'tests/unit/xir/legacy_consumers/fixtures/complete_channel_observations/close/root.xr': 'f43bb6975783d6fdceddeb6e1e018cb9cc95eddf2fd7cbf00de67c4c586333d2',
 'tests/unit/xir/legacy_consumers/fixtures/complete_channel_observations/maps/root.xr': 'b1d6ae4d8726b104ed81f6addf5423fd677694ece0057ca16dc241e011bb3b1c'}
_PROGRAMS = {'close': {'original': 'tests/regression/11_coroutine/1155_channel_close_timeout_fanout.xr',
           'selected': 'tests/unit/xir/legacy_consumers/fixtures/channel_close_distinct_from_timeout/root.xr',
           'observed': 'tests/unit/xir/legacy_consumers/fixtures/complete_channel_observations/close/root.xr'},
 'maps': {'original': 'tests/regression/11_coroutine/1166_channel_map_deep_copy_stress.xr',
          'selected': 'tests/unit/xir/legacy_consumers/fixtures/channel_map_value_copy_stress/root.xr',
          'observed': 'tests/unit/xir/legacy_consumers/fixtures/complete_channel_observations/maps/root.xr'},
 'arrays': {'original': 'tests/regression/11_coroutine/1167_channel_explicit_transfer.xr',
            'selected': 'tests/unit/xir/legacy_consumers/fixtures/channel_array_logical_values/root.xr',
            'observed': 'tests/unit/xir/legacy_consumers/fixtures/complete_channel_observations/arrays/root.xr'}}
_TRANSFORMS = {'close': [{'test': 'test_channel_close_wakes_timed_recv_fanout',
            'before': '@test\n'
                      'fn test_channel_close_wakes_timed_recv_fanout() {\n'
                      '    const waiter_count = 64\n'
                      '    const ch = Channel<i64>(0)\n'
                      '    const ready = Channel<i64>(0)\n'
                      '    const done = Channel<i64>(waiter_count)\n'
                      '\n'
                      '    for (var i = 0; i < waiter_count; i++) {\n'
                      '        go recv_timeout_until_close(ch, ready, done, 2000)\n'
                      '    }\n'
                      '\n'
                      '    for (var i = 0; i < waiter_count; i++) {\n'
                      '        assertEqual(recv_int(ready), 1)\n'
                      '    }\n'
                      '\n'
                      '    for (var i = 0; i < 16; i++) {\n'
                      '        Coro.yield()\n'
                      '    }\n'
                      '\n'
                      '    ch.close()\n'
                      '\n'
                      '    var closed_waiters = 0\n'
                      '    for (var i = 0; i < waiter_count; i++) {\n'
                      '        if (recv_int(done) == -1) {\n'
                      '            closed_waiters++\n'
                      '        }\n'
                      '    }\n'
                      '\n'
                      '    assertEqual(closed_waiters, waiter_count)\n'
                      '    ready.close()\n'
                      '    done.close()\n'
                      '}\n',
            'after': '@test\n'
                     'fn test_channel_close_wakes_timed_recv_fanout() {\n'
                     '    const waiter_count = 64\n'
                     '    const ch = Channel<i64>(0)\n'
                     '    const ready = Channel<i64>(0)\n'
                     '    const done = Channel<i64>(waiter_count)\n'
                     '\n'
                     '    for (var i = 0; i < waiter_count; i++) {\n'
                     '        go recv_timeout_until_close(ch, ready, done, 2000)\n'
                     '    }\n'
                     '\n'
                     '    for (var i = 0; i < waiter_count; i++) {\n'
                     '        assertEqual(recv_int(ready), 1)\n'
                     '    }\n'
                     '\n'
                     '    for (var i = 0; i < 16; i++) {\n'
                     '        Coro.yield()\n'
                     '    }\n'
                     '\n'
                     '    ch.close()\n'
                     '\n'
                     '    var closed_waiters = 0\n'
                     '    for (var i = 0; i < waiter_count; i++) {\n'
                     '        if (recv_int(done) == -1) {\n'
                     '            closed_waiters++\n'
                     '        }\n'
                     '    }\n'
                     '\n'
                     '    const p19Observed1 = closed_waiters\n'
                     '    assertEqual(p19Observed1, waiter_count)\n'
                     '    print(p19Observed1)\n'
                     '    ready.close()\n'
                     '    done.close()\n'
                     '}\n',
            'values': [64]}],
 'maps': [{'test': 'test_channel_map_value_copy_stress',
           'before': '@test\n'
                     'fn test_channel_map_value_copy_stress() {\n'
                     '    for (var p = 0; p < PRODUCERS; p++) {\n'
                     '        go producer(p)\n'
                     '    }\n'
                     '\n'
                     '    var total = PRODUCERS * SENDS_PER_PRODUCER\n'
                     '    var sum = 0\n'
                     '    for (var i = 0; i < total; i++) {\n'
                     '        sum = sum + recv_map_size()\n'
                     '    }\n'
                     '\n'
                     '    var completed = 0\n'
                     '    for (var p = 0; p < PRODUCERS; p++) {\n'
                     '        completed = completed + recv_done_count()\n'
                     '    }\n'
                     '\n'
                     '    assertEqual(sum, total * 2)\n'
                     '    assertEqual(completed, total)\n'
                     '\n'
                     '    maps.close()\n'
                     '    done.close()\n'
                     '}\n',
           'after': '@test\n'
                    'fn test_channel_map_value_copy_stress() {\n'
                    '    for (var p = 0; p < PRODUCERS; p++) {\n'
                    '        go producer(p)\n'
                    '    }\n'
                    '\n'
                    '    var total = PRODUCERS * SENDS_PER_PRODUCER\n'
                    '    var sum = 0\n'
                    '    for (var i = 0; i < total; i++) {\n'
                    '        sum = sum + recv_map_size()\n'
                    '    }\n'
                    '\n'
                    '    var completed = 0\n'
                    '    for (var p = 0; p < PRODUCERS; p++) {\n'
                    '        completed = completed + recv_done_count()\n'
                    '    }\n'
                    '\n'
                    '    const p19Observed2 = sum\n'
                    '    assertEqual(p19Observed2, total * 2)\n'
                    '    print(p19Observed2)\n'
                    '    const p19Observed3 = completed\n'
                    '    assertEqual(p19Observed3, total)\n'
                    '    print(p19Observed3)\n'
                    '\n'
                    '    maps.close()\n'
                    '    done.close()\n'
                    '}\n',
           'values': [40000, 20000]}],
 'arrays': [{'test': 'test_channel_send_value_preserves_source',
             'before': '@test\n'
                       'fn test_channel_send_value_preserves_source() {\n'
                       '    const ch: Channel<Array<i64>> = Channel(1)\n'
                       '    var xs = [1, 2]\n'
                       '    ch.send(xs)\n'
                       '    xs.push(3)\n'
                       '\n'
                       '    var got = recv_array(ch)\n'
                       '    got.push(99)\n'
                       '\n'
                       '    assertEqual(len(xs), 3)\n'
                       '    assertEqual(len(got), 3)\n'
                       '    assertEqual(got[0], 1)\n'
                       '    assertEqual(got[1], 2)\n'
                       '    assertEqual(got[2], 99)\n'
                       '    ch.close()\n'
                       '}\n'
                       '\n',
             'after': '@test\n'
                      'fn test_channel_send_value_preserves_source() {\n'
                      '    const ch: Channel<Array<i64>> = Channel(1)\n'
                      '    var xs = [1, 2]\n'
                      '    ch.send(xs)\n'
                      '    xs.push(3)\n'
                      '\n'
                      '    var got = recv_array(ch)\n'
                      '    got.push(99)\n'
                      '\n'
                      '    const p19Observed4 = len(xs)\n'
                      '    assertEqual(p19Observed4, 3)\n'
                      '    print(p19Observed4)\n'
                      '    const p19Observed5 = len(got)\n'
                      '    assertEqual(p19Observed5, 3)\n'
                      '    print(p19Observed5)\n'
                      '    const p19Observed6 = got[0]\n'
                      '    assertEqual(p19Observed6, 1)\n'
                      '    print(p19Observed6)\n'
                      '    const p19Observed7 = got[1]\n'
                      '    assertEqual(p19Observed7, 2)\n'
                      '    print(p19Observed7)\n'
                      '    const p19Observed8 = got[2]\n'
                      '    assertEqual(p19Observed8, 99)\n'
                      '    print(p19Observed8)\n'
                      '    ch.close()\n'
                      '}\n'
                      '\n',
             'values': [3, 3, 1, 2, 99]},
            {'test': 'test_channel_try_send_value_preserves_source',
             'before': '@test\n'
                       'fn test_channel_try_send_value_preserves_source() {\n'
                       '    const ch: Channel<Array<i64>> = Channel(1)\n'
                       '    var xs = [4, 5]\n'
                       '    assert(sent(ch.trySend(xs)))\n'
                       '    xs.push(6)\n'
                       '\n'
                       '    var got = recv_array(ch)\n'
                       '    assertEqual(len(xs), 3)\n'
                       '    assertEqual(len(got), 2)\n'
                       '    assertEqual(got[0], 4)\n'
                       '    assertEqual(got[1], 5)\n'
                       '    ch.close()\n'
                       '}\n'
                       '\n',
             'after': '@test\n'
                      'fn test_channel_try_send_value_preserves_source() {\n'
                      '    const ch: Channel<Array<i64>> = Channel(1)\n'
                      '    var xs = [4, 5]\n'
                      '    assert(sent(ch.trySend(xs)))\n'
                      '    xs.push(6)\n'
                      '\n'
                      '    var got = recv_array(ch)\n'
                      '    const p19Observed9 = len(xs)\n'
                      '    assertEqual(p19Observed9, 3)\n'
                      '    print(p19Observed9)\n'
                      '    const p19Observed10 = len(got)\n'
                      '    assertEqual(p19Observed10, 2)\n'
                      '    print(p19Observed10)\n'
                      '    const p19Observed11 = got[0]\n'
                      '    assertEqual(p19Observed11, 4)\n'
                      '    print(p19Observed11)\n'
                      '    const p19Observed12 = got[1]\n'
                      '    assertEqual(p19Observed12, 5)\n'
                      '    print(p19Observed12)\n'
                      '    ch.close()\n'
                      '}\n'
                      '\n',
             'values': [3, 2, 4, 5]},
            {'test': 'test_channel_send_timeout_value_preserves_source',
             'before': '@test\n'
                       'fn test_channel_send_timeout_value_preserves_source() {\n'
                       '    const ch: Channel<Array<i64>> = Channel(1)\n'
                       '    var xs = [7, 8]\n'
                       '    assert(sent(ch.sendTimeout(xs, 0)))\n'
                       '    xs.push(9)\n'
                       '\n'
                       '    var got = recv_array(ch)\n'
                       '    assertEqual(len(xs), 3)\n'
                       '    assertEqual(len(got), 2)\n'
                       '    assertEqual(got[0], 7)\n'
                       '    assertEqual(got[1], 8)\n'
                       '    ch.close()\n'
                       '}\n'
                       '\n',
             'after': '@test\n'
                      'fn test_channel_send_timeout_value_preserves_source() {\n'
                      '    const ch: Channel<Array<i64>> = Channel(1)\n'
                      '    var xs = [7, 8]\n'
                      '    assert(sent(ch.sendTimeout(xs, 0)))\n'
                      '    xs.push(9)\n'
                      '\n'
                      '    var got = recv_array(ch)\n'
                      '    const p19Observed13 = len(xs)\n'
                      '    assertEqual(p19Observed13, 3)\n'
                      '    print(p19Observed13)\n'
                      '    const p19Observed14 = len(got)\n'
                      '    assertEqual(p19Observed14, 2)\n'
                      '    print(p19Observed14)\n'
                      '    const p19Observed15 = got[0]\n'
                      '    assertEqual(p19Observed15, 7)\n'
                      '    print(p19Observed15)\n'
                      '    const p19Observed16 = got[1]\n'
                      '    assertEqual(p19Observed16, 8)\n'
                      '    print(p19Observed16)\n'
                      '    ch.close()\n'
                      '}\n'
                      '\n',
             'values': [3, 2, 7, 8]},
            {'test': 'test_channel_send_value_preserves_source_after_send',
             'before': '@test\n'
                       'fn test_channel_send_value_preserves_source_after_send() {\n'
                       '    const ch: Channel<Array<i64>> = Channel(1)\n'
                       '    var xs = Array<i64>(2, 0)\n'
                       '    xs[0] = 10\n'
                       '    xs[1] = 11\n'
                       '    ch.send(xs)\n'
                       '    xs.push(12)\n'
                       '    assertEqual(len(xs), 3)\n'
                       '\n'
                       '    var got = recv_array(ch)\n'
                       '    assertEqual(len(got), 2)\n'
                       '    assertEqual(got[0], 10)\n'
                       '    assertEqual(got[1], 11)\n'
                       '    ch.close()\n'
                       '}\n'
                       '\n',
             'after': '@test\n'
                      'fn test_channel_send_value_preserves_source_after_send() {\n'
                      '    const ch: Channel<Array<i64>> = Channel(1)\n'
                      '    var xs = Array<i64>(2, 0)\n'
                      '    xs[0] = 10\n'
                      '    xs[1] = 11\n'
                      '    ch.send(xs)\n'
                      '    xs.push(12)\n'
                      '    const p19Observed17 = len(xs)\n'
                      '    assertEqual(p19Observed17, 3)\n'
                      '    print(p19Observed17)\n'
                      '\n'
                      '    var got = recv_array(ch)\n'
                      '    const p19Observed18 = len(got)\n'
                      '    assertEqual(p19Observed18, 2)\n'
                      '    print(p19Observed18)\n'
                      '    const p19Observed19 = got[0]\n'
                      '    assertEqual(p19Observed19, 10)\n'
                      '    print(p19Observed19)\n'
                      '    const p19Observed20 = got[1]\n'
                      '    assertEqual(p19Observed20, 11)\n'
                      '    print(p19Observed20)\n'
                      '    ch.close()\n'
                      '}\n'
                      '\n',
             'values': [3, 2, 10, 11]},
            {'test': 'test_channel_send_helper_value_preserves_source',
             'before': '@test\n'
                       'fn test_channel_send_helper_value_preserves_source() {\n'
                       '    const ch: Channel<Array<i64>> = Channel(1)\n'
                       '    var xs = Array<i64>(2, 0)\n'
                       '    xs[0] = 12\n'
                       '    xs[1] = 13\n'
                       '    forward_array(ch, xs)\n'
                       '    xs.push(14)\n'
                       '    assertEqual(len(xs), 3)\n'
                       '\n'
                       '    var got = recv_array(ch)\n'
                       '    assertEqual(len(got), 2)\n'
                       '    assertEqual(got[0], 12)\n'
                       '    assertEqual(got[1], 13)\n'
                       '    ch.close()\n'
                       '}\n',
             'after': '@test\n'
                      'fn test_channel_send_helper_value_preserves_source() {\n'
                      '    const ch: Channel<Array<i64>> = Channel(1)\n'
                      '    var xs = Array<i64>(2, 0)\n'
                      '    xs[0] = 12\n'
                      '    xs[1] = 13\n'
                      '    forward_array(ch, xs)\n'
                      '    xs.push(14)\n'
                      '    const p19Observed21 = len(xs)\n'
                      '    assertEqual(p19Observed21, 3)\n'
                      '    print(p19Observed21)\n'
                      '\n'
                      '    var got = recv_array(ch)\n'
                      '    const p19Observed22 = len(got)\n'
                      '    assertEqual(p19Observed22, 2)\n'
                      '    print(p19Observed22)\n'
                      '    const p19Observed23 = got[0]\n'
                      '    assertEqual(p19Observed23, 12)\n'
                      '    print(p19Observed23)\n'
                      '    const p19Observed24 = got[1]\n'
                      '    assertEqual(p19Observed24, 13)\n'
                      '    print(p19Observed24)\n'
                      '    ch.close()\n'
                      '}\n',
             'values': [3, 2, 12, 13]}]}


def observations():
    def array_case(first, appended, extra=None):
        source = list(first)
        received = list(source)
        source.append(appended)
        if extra is not None:
            received.append(extra)
        return [len(source), len(received), *received]
    closed = sum(state == 'closed' for state in ['closed'] * 64)
    total_size = sum(2 for _producer in range(4) for _send in range(5000))
    completed = sum(5000 for _producer in range(4))
    return {
        'close': {'test_channel_close_wakes_timed_recv_fanout': [closed]},
        'maps': {'test_channel_map_value_copy_stress': [total_size, completed]},
        'arrays': {
            'test_channel_send_value_preserves_source': array_case([1, 2], 3, 99),
            'test_channel_try_send_value_preserves_source': array_case([4, 5], 6),
            'test_channel_send_timeout_value_preserves_source': array_case([7, 8], 9),
            'test_channel_send_value_preserves_source_after_send': array_case([10, 11], 12),
            'test_channel_send_helper_value_preserves_source': array_case([12, 13], 14)}}


def expected_rows():
    if set(_PROGRAMS) != {'close', 'maps', 'arrays'} or set(_TRANSFORMS) != set(_PROGRAMS):
        raise ValueError('complete Channel model lost a program identity')
    required = {path for program in _PROGRAMS.values()
                for path in [program['original'], program['selected'], program['observed']]}
    required |= {'stdlib/types/channel.xr', 'stdlib/types/array.xr', 'stdlib/types/map.xr',
                 'spec/source/sections/003-2-type-system.md',
                 'spec/source/sections/015-14-built-in-type-methods.md'}
    if set(_INPUTS) != required:
        raise ValueError('complete Channel model lost a source or authority binding')
    for name, digest in _INPUTS.items():
        raw = (ROOT / name).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError('complete Channel source or authority changed: ' + name)
    models = observations()
    rows = []
    for key, program in _PROGRAMS.items():
        original = (ROOT / program['selected']).read_text(encoding='utf-8')
        observed = (ROOT / program['observed']).read_text(encoding='utf-8')
        rebuilt = original
        for change in _TRANSFORMS[key]:
            if rebuilt.count(change['before']) != 1:
                raise ValueError('Channel observation does not preserve one complete assertion')
            rebuilt = rebuilt.replace(change['before'], change['after'])
        if observed != rebuilt:
            raise ValueError('Channel instrumentation changed a complete original body')
        names = re.findall(r'@test\s+fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(', observed)
        per_test = models[key]
        if names != list(per_test):
            raise ValueError('Channel model lost or reordered a required named body')
        transform_values = {name: [value for change in _TRANSFORMS[key]
                                  if change['test'] == name for value in change['values']]
                            for name in names}
        if transform_values != per_test:
            raise ValueError('Channel assertion observation differs from independent values')
        stdout = ''.join(str(value) + '\n' for values in per_test.values() for value in values)
        rows.append({'id': 'complete_channel_named_' + key, 'source': program['observed'],
                     'original_source': program['original'], 'required_target': None,
                     'expected': {'exit_code': 0, 'stdout_utf8': stdout, 'stderr_utf8': '',
                                  'required_test_names': names,
                                  'per_test_observations': {name: {'values': values}
                                                            for name, values in per_test.items()}},
                     'model': 'All seven complete named bodies retain their assertions, pressure, timeout and cleanup. Read each selected terminal scalar assertion operand once, assert it, then print it. Separate real CLI discovery, VM and native execution are required; no FI or physical cleanup qualification follows from these observations.',
                     'observation_count': sum(map(len, per_test.values()))})
        rows.append({'id': 'complete_channel_standalone_' + key, 'source': program['selected'],
                     'original_source': program['original'], 'required_target': None,
                     'expected': {'exit_code': 0, 'stdout_utf8': '', 'stderr_utf8': ''},
                     'model': 'The original standalone route declares named bodies without calling them. Empty stdout requires successful actual execution but grants no named-body, Channel transfer, scheduling, allocation or cleanup qualification.',
                     'observation_count': 0})
    return rows
