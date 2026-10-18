"""Independent schema32/contract91 Generator fields and immutable retired packets.

The model encodes complete current Module fields directly. It reads only
source declarations to pin the fixed ordinals; no product writer or executable
supplies expected packet bytes. Retired packet90 models and bytes stay intact.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
from derive_construction72_vectors import frame, header
from derive_generator90_vectors import packet as packet90

ROOT = Path(__file__).resolve().parent
CURRENT = (32, 91)
NAMES = ('i64', 'unit', 'no_role', 'scalar_result', 'wrong_value', 'return_arg',
         'no_suspend', 'ref_parameter', 'unit_arg', 'unknown_role',
         'initializer_role', 'entry_role', 'map_result', 'set_result')
HISTORICAL_NAMES = NAMES
# Preserve the semantic expectations recorded when packet90 was current.
HISTORICAL_EXPECTATIONS = ('OK', 'OK', 'BAD_TYPE', 'BAD_TYPE', 'BAD_TYPE',
                           'BAD_STRUCTURE', 'BAD_STRUCTURE', 'BAD_TYPE',
                           'BAD_STRUCTURE', 'BAD_STRUCTURE', 'BAD_STRUCTURE',
                           'BAD_STRUCTURE', 'BAD_TYPE', 'BAD_TYPE')


def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def named(*fields):
    return words(*(value for name, value in fields))


def blob(text):
    data = text.encode('utf-8')
    return words(len(data)) + data


def instruction(op, result=0, left=0, immediate=0):
    return words(op, result, left, 0, 0, 0) + struct.pack('<q', immediate) + words(0, 0)


def function(name, result, code, parameters=()):
    # One complete block: first, count, panic target and cleanup frontier.
    return (blob(name) + words(len(parameters), *parameters, result, 1,
                              0, len(code), 0, 0, len(code)) +
            b''.join(code) + words(0))


def identity(exported=0, promises=0):
    return named(('module', 0), ('exported', exported), ('nominal_owner', 0),
                 ('member_access', 0), ('cleanup_owner', 0), ('promises', promises),
                 ('method_kind', 0), ('test_role', 0), ('test_timeout_seconds', 0))


def packet(variant):
    assert variant in NAMES
    unit = variant in ('unit', 'unit_arg')
    promises = 0 if variant == 'no_role' else 3 if variant == 'no_suspend' else (
        0xffffffff if variant == 'unknown_role' else 2)
    parameters = (257,) if variant == 'ref_parameter' else ()
    code = ([instruction(184, left=int(variant == 'unit_arg')), instruction(184)] if unit else
            [instruction(1 if variant == 'wrong_value' else 2,
                         1 if variant == 'wrong_value' else 2,
                         immediate=1 if variant == 'wrong_value' else 11),
             instruction(184, left=len(parameters))])
    code.append(instruction(33, left=int(variant == 'return_arg')))
    body = named(('linkage_program', 0), ('function_count', 3), ('declarations_present', 1))
    body += function('init', 0, [instruction(33)])
    body += function('units' if unit else 'numbers',
                     2 if variant == 'scalar_result' else 256, code, parameters)
    body += function('entry', 2, [instruction(2, 2), instruction(33)])
    body += named(('module_count', 1), ('slot_count', 0), ('literal_count', 0),
                  ('root_module', 0), ('entry_function', 2))
    body += blob('generator') + named(('dependency_count', 0), ('initializer', 0),
                                      ('lexical_scope', 0), ('storage', 0))
    body += identity(promises=2 if variant == 'initializer_role' else 0)
    body += identity(exported=1, promises=promises)
    body += identity(promises=2 if variant == 'entry_role' else 0)
    body += named(('implementation_count', 0), ('generics_present', 0),
                  ('type_count', 2 if parameters else 1), ('nominal_count', 0),
                  ('interface_count', 0))
    element = 0 if unit else 2
    # Map and Set are admitted descriptors, but neither is a Generator result.
    if variant == 'map_result':
        body += words(9, 0, 2, 2)
    elif variant == 'set_result':
        body += words(10, 0, 2)
    else:
        body += words(13, 0, element)
    if parameters:
        body += words(3, 0, 2)
    body += named(('construction_rows', 0), ('materialization_bindings', 0),
                  ('comptime_sites', 0), ('comptime_bindings', 0),
                  ('defaults', 0), ('provenance_absent', 0))
    return frame(body, *CURRENT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    source = ROOT.parents[2]
    checked = (source / 'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    for macro, value in zip(('SCHEMA', 'CONTRACT'), CURRENT):
        assert re.search(r'^#define XR_XIR_CHECKED_' + macro + ' ' + str(value) + r'u$', checked, re.M)
    types = (source / 'src/xir/xxir.h').read_text(encoding='utf-8')
    assert re.search(r'XR_XIR_TYPE_ITERATOR\s*=\s*13\b', types)
    ops = re.findall(r'^XR_XIR_OP\((\w+),',
                     (source / 'src/xir/xxir_ops.def').read_text(encoding='utf-8'), re.M)
    assert ops.index('CONST_BOOL') + 1 == 1 and ops.index('CONST_INT') + 1 == 2
    assert ops.index('RETURN') + 1 == 33 and ops.index('GENERATOR_YIELD') + 1 == 184
    old = [('generator90_' + name, packet90(name)) for name in HISTORICAL_NAMES]
    header(ROOT / 'xir_generator90_golden.h', 'XIR_GENERATOR90_GOLDEN_H', old, False)
    current = [('generator91_' + name, packet(name)) for name in NAMES]
    header(ROOT / 'xir_generator91_golden.h', 'XIR_GENERATOR91_GOLDEN_H', current, args.write)
    print(json.dumps(dict(wire=CURRENT[0], contract=CURRENT[1], product_writer_used=False,
                          named_fields=True, historical_packets=len(old),
                          historical=[dict(name=name, expected_when_current=expected,
                                           retired_receiving='BAD_STRUCTURE_NO_ALLOCATION',
                                           sha256=hashlib.sha256(data).hexdigest())
                                      for (name, data), expected in zip(old, HISTORICAL_EXPECTATIONS)],
                          current=[dict(name=name, bytes=len(data),
                                        sha256=hashlib.sha256(data).hexdigest())
                                   for name, data in current]), indent=2))


if __name__ == '__main__':
    main()
