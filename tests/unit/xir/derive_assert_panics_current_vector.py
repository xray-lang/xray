"""Frame the current Core packet from the complete fixed declaration model."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, re


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    root = directory.parents[2]
    checked = (root / 'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'^#define XR_XIR_CHECKED_SCHEMA 28u$', checked, re.M)
    assert re.search(r'^#define XR_XIR_CHECKED_CONTRACT 76u$', checked, re.M)
    utility = load(directory / 'derive_construction72_vectors.py', 'core_current_framing')
    model = load(directory / 'derive_assert_panics71_vector.py', 'core_current_fields')
    body, offsets = model.core_body(model.OPCODES, root_upper=8, template=True)
    insertion = offsets['defaults'] - 64
    body = body[:insertion] + bytes(4) + body[insertion:]
    previous = {}
    for wire, semantic in ((26, 71), (27, 72), (28, 73), (28, 75)):
        prior_body = body[:insertion] + body[insertion + 4:] if wire == 26 else body
        packet = utility.frame(prior_body, wire, semantic)
        assert packet == utility.literal(directory / ('xir_assert_panics' + str(semantic) + '_golden.h'),
                                         'assert_panics' + str(semantic) + '_golden')
        previous[str(semantic)] = hashlib.sha256(packet).hexdigest()
    current = utility.frame(body, 28, 76)
    assert len(current) == 2187 and len(body) == 2123
    macros = [('XR_PANICS_CURRENT_VECTOR_' + name.upper(), value + (4 if value >= insertion + 64 else 0))
              for name, value in offsets.items()]
    macros.append(('XR_PANICS_CURRENT_CALLABLE_FLAGS', 1915))
    utility.header(directory / 'xir_assert_panics_current_golden.h', 'XIR_ASSERT_PANICS_CURRENT_GOLDEN_H',
                   [('assert_panics_current_golden', current)], args.write, macros)
    print(json.dumps(dict(wire=28, semantic=76, bytes=len(current),
                         sha256=hashlib.sha256(current).hexdigest(), body_sha256=hashlib.sha256(body).hexdigest(),
                         complete_previous_packets=previous, product_writer_used=False,
                         channel_instructions_in_core=False, runtime_test_required=True), indent=2))


if __name__ == '__main__':
    main()
