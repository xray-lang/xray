"""Derive complete Task wire vectors from named fields without a product writer.

Schema 28 changes Error-constrained nominal metadata. These Task models have
no nominals and no generic constraints, so the explicit schema 27 payload is
unchanged. Reproduce both complete schema 27 vectors before framing schema 28;
historical bytes are assertions, never inputs to the current payload model.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    test = args.root / 'tests/unit/xir'
    model = load(test / 'derive_task_checked70_goldens.py', 'task_named_fields')
    fields = load(test / 'derive_construction72_vectors.py', 'task_complete_framing')
    rows = []
    for kind, body70 in [('unit', model.types_body(model.UNIT)), ('go', model.go_body('golden'))]:
        assert body70[-8:] == bytes(8)
        body = body70[:-8] + bytes(4) + body70[-8:]
        previous = fields.frame(body, 27, 72)
        old = test / ('xir_task_' + kind + '72_golden' + ('_part1' if kind == 'go' else '') + '.h')
        assert previous == fields.literal(old, 'task_' + kind + '72_golden')
        packet = fields.frame(body, 28, 73)
        assert packet[64:] == previous[64:]
        expected = 225 if kind == 'unit' else len(previous)
        assert len(packet) == expected
        path = test / ('xir_task_' + kind + '73_golden.h')
        fields.header(path, 'XIR_TASK_' + kind.upper() + '73_GOLDEN_H',
                      [('task_' + kind + '73_golden', packet)], args.write)
        rows.append({'kind': kind, 'bytes': len(packet),
                     'previous_sha256': hashlib.sha256(previous).hexdigest(),
                     'current_sha256': hashlib.sha256(packet).hexdigest(),
                     'body_sha256': hashlib.sha256(body).hexdigest()})
    print(json.dumps({'status': 'INDEPENDENT_COMPLETE_TASK_FIELDS_28_73',
                      'writer_or_executable_used': False, 'historical_payload_copied': False,
                      'schema': 28, 'semantic': 73, 'rows': rows}, indent=2))


if __name__ == '__main__':
    main()
