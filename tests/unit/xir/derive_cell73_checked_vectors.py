"""Frame seven current common packets from complete independent field models.

Every preceding 27/72 packet is reproduced completely before the new 28/73
frame is derived. Neither product writer output nor an executable is read.
"""
from pathlib import Path
import argparse, hashlib, importlib.util, json, struct


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    src = args.source_root / 'tests/unit/xir'
    out = args.output_root / 'tests/unit/xir'
    utility = load(src / 'derive_construction72_vectors.py', 'identity_common_framing')
    callable_model = load(src / 'derive_callable71_checked_vectors.py', 'identity_common_callable')
    body, _ = callable_model.encode_body(callable_model.MODELS['8'], 8)
    assert body[-8:] == bytes(8)
    models = [('scalar', 'checked_scalar', body[:-8] + bytes(4) + body[-8:], ())]
    panics = load(src / 'derive_assert_panics71_vector.py', 'identity_common_panics')
    body, offsets = panics.core_body(panics.OPCODES, root_upper=8, template=True)
    insert = offsets['defaults'] - 64
    macros = [('XR_PANICS73_VECTOR_' + name.upper(), value + (4 if value >= offsets['defaults'] else 0))
              for name, value in offsets.items()] + [('XR_PANICS73_CALLABLE_FLAGS', 1915)]
    models.append(('panics', 'assert_panics', body[:insert] + bytes(4) + body[insert:], macros))
    for stem, script, arguments in [
        ('range', 'derive_range_checked70_goldens.py', (70,)),
        ('rune', 'derive_rune_checked_golden.py', (25, 65, 136, 137, 33)),
    ]:
        model = load(src / script, 'identity_common_' + stem)
        prior = model.packet(*arguments); body = prior[64:]
        assert body[-8:] == bytes(8)
        models.append((stem, 'checked_range' if stem == 'range' else 'rune',
                       body[:-8] + bytes(4) + body[-8:], ()))
    records = []
    for stem, symbol, body, macros in models:
        basename = {'scalar': 'xir_checked_scalar', 'panics': 'xir_assert_panics',
                    'range': 'xir_checked_range', 'rune': 'xir_rune'}[stem]
        old = utility.frame(body, 27, 72)
        assert old == utility.literal(src / (basename + '72_golden.h'), symbol + '72_golden')
        current = utility.frame(body, 28, 73)
        assert old[64:] == current[64:] and len(old) == len(current)
        assert struct.unpack('<II', current[8:16]) == (28, 73)
        assert hashlib.sha256(current[:32] + current[64:]).digest() == current[32:64]
        rows = [(symbol + '73_golden', current)]
        if stem == 'scalar': rows.append((symbol + '73_digest', current[32:64]))
        utility.header(out / (basename + '73_golden.h'), basename.upper() + '73_GOLDEN_H',
                       rows, args.write, macros)
        records.append(dict(name=symbol, bytes=len(current), old72_sha256=hashlib.sha256(old).hexdigest(),
                            current73_sha256=hashlib.sha256(current).hexdigest(),
                            complete_body_sha256=hashlib.sha256(body).hexdigest()))
    rows = []
    for suffix, upper in [('golden', 8), ('invalid_flags0', 0), ('invalid_flags1', 1)]:
        body, offsets = callable_model.encode_body(callable_model.MODELS['14'], upper)
        assert offsets == [437] and body[-8:] == bytes(8)
        body = body[:-8] + bytes(4) + body[-8:]
        old = utility.frame(body, 27, 72)
        symbol = 'generic_method72_' + suffix
        assert old == utility.literal(src / 'xir_generic_method72_golden.h', symbol)
        current = utility.frame(body, 28, 73)
        assert current[64:] == old[64:] and len(current) == 620
        assert current.index(b'map') + 3 + 8 == 544
        assert current.index(b'copy') + 4 + 8 == 584
        rows.append(('generic_method73_' + suffix, current))
        records.append(dict(name=rows[-1][0], bytes=len(current),
                            old72_sha256=hashlib.sha256(old).hexdigest(),
                            current73_sha256=hashlib.sha256(current).hexdigest(),
                            complete_body_sha256=hashlib.sha256(body).hexdigest()))
    utility.header(out / 'xir_generic_method73_golden.h', 'XIR_GENERIC_METHOD73_GOLDEN_H',
                   rows, args.write, [('GENERIC_METHOD73_MAP_OWN', 544),
                                      ('GENERIC_METHOD73_COPY_OWN', 584)])
    print(json.dumps(dict(status='INDEPENDENT_COMMON28_73_MODELS_ONLY', records=records,
                         previous_full_models_reproduced=True, product_writer_used=False,
                         executable_read=False, runtime_qualification='NOT_RUN'), indent=2))


if __name__ == '__main__': main()
