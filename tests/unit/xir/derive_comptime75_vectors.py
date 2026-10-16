"""Derive complete current packet fields, preserving every historical vector.

Only independent named field models are inputs. Two mandatory zero site and binding counts follow the literal-edge catalogue,
+including in recursive Source modules. Every prior packet is reconstructed first.
No compiler writer, executable or runtime-emitted packet is read.
"""
from pathlib import Path
import argparse
import copy
import hashlib
import importlib.util
import json
import struct


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    src = args.source_root / 'tests/unit/xir'
    out = args.output_root / 'tests/unit/xir'
    fields = load(src / 'derive_construction72_vectors.py', 'literal_fields')
    callable_model = load(src / 'derive_callable71_checked_vectors.py', 'literal_callable')
    words = lambda *values: struct.pack('<' + 'I' * len(values), *values)
    records = []

    def emit(basename, symbol, old_body, insertion, macros=(), previous=False):
        old = fields.frame(old_body, 28, 73)
        assert old == fields.literal(src / (basename + '73_golden.h'), symbol + '73_golden')
        prior_body = old_body[:insertion] + words(0) + old_body[insertion:]
        prior = fields.frame(prior_body, 29, 74)
        assert prior == fields.literal(src / (basename + '74_golden.h'), symbol + '74_golden')
        current = fields.frame(prior_body[:insertion+4] + words(0,0) + prior_body[insertion+4:], 30, 75)
        rows = [(symbol + '75_golden', current)]
        if symbol == 'checked_scalar':
            rows.append((symbol + '75_digest', current[32:64]))
        if previous:
            rows.append((symbol + '73_previous', old))
        fields.header(out / (basename + '75_golden.h'), basename.upper() + '75_GOLDEN_H', rows, args.write, macros)
        records.append(dict(name=symbol, previous73_bytes=len(old), current75_bytes=len(current),
                            previous73_sha256=hashlib.sha256(old).hexdigest(), sha256=hashlib.sha256(current).hexdigest(),
                            site_count_offset=68 + insertion))

    body, _ = callable_model.encode_body(callable_model.MODELS['8'], 8)
    body = body[:-8] + words(0) + body[-8:]
    emit('xir_checked_scalar', 'checked_scalar', body, len(body)-8)
    for stem, script, arguments in [('range', 'derive_range_checked70_goldens.py', (70,)),
                                    ('rune', 'derive_rune_checked_golden.py', (25, 65, 136, 137, 33))]:
        model = load(src / script, 'literal_' + stem)
        body = model.packet(*arguments)[64:]
        body = body[:-8] + words(0) + body[-8:]
        emit('xir_checked_range' if stem == 'range' else 'xir_rune',
             'checked_range' if stem == 'range' else 'rune', body, len(body)-8)
    panics = load(src / 'derive_assert_panics71_vector.py', 'literal_panics')
    body, offsets = panics.core_body(panics.OPCODES, root_upper=8, template=True)
    position = offsets['defaults']-64
    body = body[:position] + words(0) + body[position:]
    macros = [('XR_PANICS75_VECTOR_' + name.upper(), value + (16 if value >= offsets['defaults'] else 0))
              for name, value in offsets.items()] + [('XR_PANICS75_CALLABLE_FLAGS', 1915)]
    emit('xir_assert_panics', 'assert_panics', body, position+4, macros)
    rows = []
    for suffix, upper in [('golden', 8), ('invalid_flags0', 0), ('invalid_flags1', 1)]:
        body, _ = callable_model.encode_body(callable_model.MODELS['14'], upper)
        body = body[:-8] + words(0) + body[-8:]
        old = fields.frame(body, 28, 73)
        assert old == fields.literal(src / 'xir_generic_method73_golden.h', 'generic_method73_' + suffix)
        prior_body = body[:-8] + words(0) + body[-8:]
        assert fields.frame(prior_body,29,74) == fields.literal(src / 'xir_generic_method74_golden.h', 'generic_method74_' + suffix)
        current = fields.frame(prior_body[:-8] + words(0,0) + prior_body[-8:], 30,75)
        rows.append(('generic_method75_' + suffix, current))
    fields.header(out / 'xir_generic_method75_golden.h', 'XIR_GENERIC_METHOD75_GOLDEN_H', rows, args.write,
                  [('GENERIC_METHOD75_MAP_OWN', 544), ('GENERIC_METHOD75_COPY_OWN', 584)])
    error = load(src / 'derive_error_marker73_checked_vector.py', 'literal_error')
    body, _ = callable_model.encode_body(error.MODEL, 0)
    body = body[:-8] + words(0) + body[-8:]
    emit('xir_error_marker', 'error_marker', body, len(body)-8,
         [('ERROR_MARKER75_KINDS_PRESENT', 169), ('ERROR_MARKER75_MARKER', 173)], True)
    sizing = load(src / 'compile_owner/derive_checked_sizing73_golden.py', 'literal_sizing')
    rows = []
    for suffix, name in [('golden', b'main'), ('embedded_golden', b'm\0in'), ('empty_golden', b'')]:
        old = sizing.packet(name, 28, 73, 33, True)
        assert old == sizing.literal(src / 'compile_owner/checked_sizing73_golden.h', 'sizing73_' + suffix)
        body = old[64:]
        prior_body = body[:-8] + words(0) + body[-8:]
        assert fields.frame(prior_body,29,74) == fields.literal(src / 'compile_owner/checked_sizing74_golden.h', 'sizing74_' + suffix)
        rows.append(('sizing75_' + suffix, fields.frame(prior_body[:-8] + words(0,0) + prior_body[-8:], 30,75)))
    fields.header(out / 'compile_owner/checked_sizing75_golden.h', 'CHECKED_SIZING75_GOLDEN_H', rows, args.write)
    root_model = load(src / 'derive_root_construction72_vectors.py', 'literal_nominal')
    model = copy.deepcopy(root_model.model())
    model['nodes'][0][4] = [2, 2]
    op = root_model.instruction
    fn = root_model.function
    model['functions'] = [model['functions'][0],
        fn('entry', 2, [op(28, 256, immediate=2), op(28, 2, (0, 1), 3), op(33, args=(1, 0))], [0]),
        fn('make', 256, [op(2, 2, immediate=19), op(2, 2, immediate=23),
                         op(82, 256, (0, 2)), op(33, args=(2, 0))], [0, 1]),
        dict(name='sum', parameters=[256], result=2, blocks=[[0, 4, 0, 0]],
             instructions=[op(83, 2), op(83, 2, immediate=1), op(25, 2, (1, 2)), op(33, args=(3, 0))], operands=[])]
    model['declarations']['identities'] = [[0,0,0,0,0,0,0,0,0], [0,1,0,0,0,0,0,0,0],
                                          [0,0,1,0,0,0,5,0,0], [0,1,1,0,0,0,2,0,0]]
    for field in model['nominals'][0][-2]:
        field[2] = 1
    body, _ = callable_model.encode_body(model, 0)
    assert body[-8:] == bytes(8)
    construction = words(1, 0, 2, 0, 0)
    catalogue = words(1, 1, 0, 2, 0)
    template = words(1,4)
    for f, function in enumerate(model['functions']):
        template += words(len(function['parameters'])) + words(0,0)*len(function['parameters'])
        template += words(4 if f==0 else 0,0,0,0)
    body = body[:-8] + construction + catalogue + words(0) + template
    assert fields.frame(body,29,74) == fields.literal(src / 'materialization74_golden.h', 'materialization74_golden')
    body = body[:-len(template)-4] + words(0,0) + body[-len(template)-4:]
    materializer = fields.frame(body, 30,75)
    fields.header(out / 'materialization75_golden.h', 'MATERIALIZATION75_GOLDEN_H',
                  [('materialization75_golden', materializer)], args.write)
    records.append(dict(name='private_bound_materializer', bytes=len(materializer),
                        sha256=hashlib.sha256(materializer).hexdigest(), catalogue=[dict(caller=1,instruction=0,function=2,nominal=0)]))
    print(json.dumps(dict(status='INDEPENDENT_COMPLETE_FIELDS_ONLY', records=records,
                         historical73_reproduced=True, product_writer_used=False, executable_read=False), indent=2))


if __name__ == '__main__':
    main()
