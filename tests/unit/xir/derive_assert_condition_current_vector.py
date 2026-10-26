"""Fixed condition fields for the existing Checked 37/100 contract.

Historical packets are independent comparisons. No compiler, artifact writer,
runtime executable or writer dump supplies any expected byte.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import struct


def load(path):
    spec = importlib.util.spec_from_file_location('condition_framing', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def instruction(op, result=0, left=0, right=0, immediate=0):
    return words(op, result, left, right, 0, 0) + struct.pack('<q', immediate) + words(0, 0)


def condition_body(current):
    # Unit0, Bool1, String3, Error14, Never17; RETURN33 and CONST_STRING3 retain roles.
    # The historical condition instruction123 is current instruction121.
    body = words(1, 3, 1)
    functions = (
        ('$init', (), 0, 14, (instruction(33),)),
        ('assert', (1, 3), 0, 17, (instruction(121 if current else 123, right=1), instruction(33))),
        ('$argument_default', (), 3, 17, (instruction(3, 3), instruction(33))),
    )
    for name, parameters, result, error, instructions in functions:
        name = name.encode('ascii')
        body += words(len(name)) + name + words(len(parameters), *parameters, result)
        if current:
            body += words(error)  # Module Error14; declaration/default Never17.
        body += words(1, 0, len(instructions), 0, 0, len(instructions))
        body += b''.join(instructions) + words(0)
    # Entryless Library: one fixed factory module and one empty STRING literal.
    identity = b'memory-module-v1:id=23:xray-core-assertions-v1'
    body += words(1, 0, 1, 0xffffffff, 0xffffffff)
    body += words(len(identity)) + identity + words(0, 0)
    if current:
        body += words(0, 0)  # Own lexical scope and fresh storage.
    for exported in (0, 1, 0):
        body += words(0, exported, 0, 0, 0, 0, 0, 0, 0)
    body += words(0, 0)  # Empty literal and implementation count.
    body += words(0, 0, 0, 0)  # No generic/type/nominal/interface tables.
    body += words(0)  # Dense nominal construction denominator.
    if current:
        # Materializations, comptime sites/bindings and complete slot facts.
        body += words(0, 0, 0, 0)
    body += words(1, 0, 1, 1, 2)  # Parameter owner1, ordinal1, private helper2.
    body += words(0)  # No source-template or specialization provenance.
    return body


def verify_current(source_root=None, output_root=None, write=False):
    directory = Path(__file__).resolve().parent
    root = Path(source_root) if source_root else directory.parents[2]
    target = Path(output_root) / 'tests/unit/xir' if output_root else root / 'tests/unit/xir'
    source = root / 'tests/unit/xir'
    checked = (root / 'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'^#define XR_XIR_CHECKED_SCHEMA 37u$', checked, re.M)
    assert re.search(r'^#define XR_XIR_CHECKED_CONTRACT 100u$', checked, re.M)
    utility = load(source / 'derive_construction72_vectors.py')
    previous = utility.frame(condition_body(False), 27, 72)
    assert len(previous) == 670
    assert previous == utility.literal(source / 'xir_assert_condition72_golden.h',
                                       'assert_condition72_golden')
    current = utility.frame(condition_body(True), 37, 100)
    assert len(current) == 706
    utility.header(target / 'xir_assert_condition_current_golden.h',
                   'XIR_ASSERT_CONDITION_CURRENT_GOLDEN_H',
                   [('assert_condition_current_golden', current)], write)
    return dict(wire=37, semantic=100, bytes=len(current),
                sha256=hashlib.sha256(current).hexdigest(),
                reproduced_previous27_72_sha256=hashlib.sha256(previous).hexdigest(),
                production_writer_used=False, runtime_match_required=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path)
    parser.add_argument('--output-root', type=Path)
    parser.add_argument('--write', action='store_true')
    options = parser.parse_args()
    print(json.dumps(verify_current(options.source_root, options.output_root,
                                    options.write), indent=2))
