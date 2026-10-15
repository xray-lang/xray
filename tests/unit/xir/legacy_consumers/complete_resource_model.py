"""Independent arithmetic and the original required counter postconditions."""
import hashlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'stdlib/runtime/runtime.c': 'e969dd60094f379950636359499ad2951eca95062c89dcae1d29857dc0c08e4f',
 'stdlib/runtime/runtime.xr': '9f4d87664a727e7b29ceec161efe07d67d4e416c47030b06c682062f8f2fe57c',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'tests/diff/cases/semantics/oop/native_class_reference_field_transfer.xr': 'af69cc337eaed2096f10bcb09e18c2cdc85dfe5835c962042962067f9cd176b3',
 'tests/unit/xir/legacy_consumers/fixtures/execution_ports/native_class_reference_field_transfer/root.xr': 'f7d2e7d94790ea0e4d6d88a8305a51fe4f0062e78cc727b690bc9d6f06b24340'}
_ORIGINAL = 'tests/diff/cases/semantics/oop/native_class_reference_field_transfer.xr'
_SELECTED = 'tests/unit/xir/legacy_consumers/fixtures/execution_ports/native_class_reference_field_transfer/root.xr'


def expected_rows():
    required = {_ORIGINAL, _SELECTED, 'stdlib/runtime/runtime.xr', 'stdlib/runtime/runtime.c',
                'stdlib/time/time.xr', 'spec/source/sections/003-2-type-system.md'}
    if set(_INPUTS) != required:
        raise ValueError('complete resource model lost its source or public declaration')
    for name, digest in _INPUTS.items():
        raw = (ROOT / name).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError('complete resource source or authority changed: ' + name)
    payload = [7] * 4096
    total = sum(len(payload) for _round in range(2000))
    packet = {'payload': [1]}
    packet['payload'] = [2, 3]
    packet['payload'] = [4, 5, 6]
    packet['payload'] = list(packet['payload'])
    # These two booleans are required original regression postconditions.
    # No exact counter reading or physical allocation count is inferred.
    predicates = [
        {'measurement': 'liveBytes - beforeBytes', 'comparison': '<', 'limit': 262144, 'required': True},
        {'measurement': 'liveObjects - beforeObjects', 'comparison': '<', 'limit': 128, 'required': True}]
    values = [total, len(packet['payload']), *(item['required'] for item in predicates)]
    render = lambda value: str(value).lower() if type(value) is bool else str(value)
    return [{'id': 'complete_resource_class_field', 'source': _SELECTED,
             'original_source': _ORIGINAL, 'required_target': None,
             'expected': {'exit_code': 0, 'stdout_utf8': ''.join(render(v) + '\n' for v in values),
                          'stderr_utf8': ''},
             'model': 'Independent4096*2000 arithmetic and final[4,5,6] field assignment, plus the two unchanged required bounded-counter postconditions. Retain full pressure, sleeps, field replacement and self-assignment. Counter predicates do not prove physical reclamation, allocator zero, cleanup under failure, complete FI, resources, cancellation or sanitizers.',
             'observation_count': 4, 'counter_postconditions': predicates,
             'qualification_scope': 'NORMAL_OUTPUT_AND_ORIGINAL_COUNTER_PREDICATES_ONLY',
             'physical_reclamation_qualification': 'OPEN_REQUIRES_COMPLETE_PRODUCER_GATES'}]
