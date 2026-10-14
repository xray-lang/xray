"""Independent single-thread Atomic transitions and nominal enum catch results."""
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]


def expected_rows() -> list[dict]:
    bindings = json.loads((HERE / 'extended_named_bindings.json').read_text(encoding='utf-8'))
    for item in bindings['inputs']:
        if hashlib.sha256((ROOT / item['path']).read_bytes().replace(b'\r\n', b'\n')).hexdigest() != item['lf_sha256']:
            raise ValueError('extended named source or authority changed: ' + item['path'])
    cases = {
        'atomic_original_named_tests_observed': {
            'test_atomic_int_load_store': [0, 42],
            'test_atomic_int_add_sub': [10 + 5, 10 + 5 - 3],
            'test_atomic_int_fetch_add_sub': [10, 10 + 5, 10 + 5, 10 + 5 - 3],
            'test_atomic_int_swap': [1, 99],
            'test_atomic_int_compare_exchange_success': [0, True, 1],
            'test_atomic_int_compare_exchange_failure': [False, 5, 5],
            'test_atomic_bool_load_store': [False, True],
            'test_atomic_bool_toggle': [False, True, True, False],
            'test_atomic_float_load_store': [0.0, 3.14],
            'test_atomic_float_add_sub': [1.0 + .5, 1.0 + .5 - .25],
            'test_atomic_to_string': ['42'],
            'test_atomic_ordering_enum': [1, 2],
        },
        'typed_enum_filter_original_named_tests_observed': {
            'test_typed_enum_catch_first': ['net'],
            'test_typed_enum_catch_second': ['disk'],
            'test_typed_enum_no_throw': ['none'],
            'test_typed_enum_then_catchall_match': ['net'],
            'test_typed_enum_then_catchall_fallthrough': ['other'],
        },
    }
    return [{'id': name, 'expected': {'exit_code': 0, 'required_test_names': list(models),
                                     'per_test_observations': {key: {'values': values} for key, values in models.items()}},
             'model_scope': 'Source-derived transitions only; no memory-order, parallel atomicity, FI or safety qualification.'}
            for name, models in cases.items()]
