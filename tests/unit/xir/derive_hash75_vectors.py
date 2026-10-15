"""Derive complete hash-family packets and current readers from named fields.

The current common bodies are reconstructed by their existing independent field
models and matched to complete retired74 inputs before framing75. No compiler,
product writer, generated C or runtime measurement supplies an expected byte.
"""
from pathlib import Path
import argparse, contextlib, copy, hashlib, importlib.util, io, json, re, struct, sys, tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def load(name, path=None):
    spec = importlib.util.spec_from_file_location(name, path or HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def arrays(path):
    return [(m[1], bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', m[2])))
            for m in re.finditer(r'static const uint8_t (\w+)\[[^]]*\]\s*=\s*\{(.*?)\};',
                                 path.read_text(encoding='utf-8'), re.S)]


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    fields = load('derive_construction72_vectors')
    records = []
    with tempfile.TemporaryDirectory(prefix='hash75-fields-') as temporary:
        for script in ['derive_string74_checked_vectors', 'derive_error_marker74_checked_vector']:
            saved = sys.argv
            try:
                sys.argv = [script, '--source-root', str(ROOT), '--output-root', temporary, '--write']
                with contextlib.redirect_stdout(io.StringIO()): load(script).main()
            finally: sys.argv = saved
        for previous in sorted((Path(temporary) / 'tests/unit/xir').glob('*74_golden.h')):
            fixed = HERE / previous.name
            assert previous.read_bytes() == fixed.read_bytes(), previous.name
            rows = []
            for name, value in arrays(previous):
                if '74' not in name: continue
                if value.startswith(b'XRCHK\0\0\0'):
                    assert struct.unpack_from('<II', value, 8) == (28, 74)
                    value = fields.frame(value[64:], 28, 75)
                elif name.endswith('digest'):
                    continue
                rows.append((name.replace('74', '75'), value))
                if name == 'checked_scalar74_golden': rows.append(('checked_scalar75_digest', value[32:64]))
            macros = [(name.replace('74', '75'), int(value)) for name, value in
                      re.findall(r'#define (\w*74\w*) (\d+)u?', previous.read_text())]
            target = HERE / previous.name.replace('74', '75')
            fields.header(target, target.stem.upper() + '_H', rows, args.write, macros)
            records += [{'header': target.name, 'symbol': n, 'bytes': len(v),
                         'sha256': hashlib.sha256(v).hexdigest()} for n, v in rows]
    sizing = load('hash_sizing_fields', HERE / 'compile_owner/derive_checked_sizing74_golden.py')
    rows = []
    for suffix, name in [('golden', b'main'), ('embedded_golden', b'm\0in'), ('empty_golden', b'')]:
        old = sizing.packet(name, 28, 74, 33, True)
        assert old == sizing.literal(HERE / 'compile_owner/checked_sizing74_golden.h', 'sizing74_' + suffix)
        rows.append(('sizing74_retired_' + suffix, old))
        rows.append(('sizing75_' + suffix, sizing.packet(name, 28, 75, 33, True)))
    fields.header(HERE / 'compile_owner/checked_sizing75_golden.h', 'CHECKED_SIZING75_GOLDEN_H', rows, args.write)
    encoder = load('derive_callable71_checked_vectors')
    # One definition returns its borrowed parameter through the normal
    # owned result path. Every type edge and zero denominator is explicit.
    model = dict(linkage=0, functions=[dict(name='echo', parameters=[257], result=257,
        blocks=[[0, 1, 0, 0]], instructions=[[33, 0, 0, 0, 0, 0, 0, 0, 0]], operands=[])],
        declarations=None, generics=None, nodes=[[2, 0, 2], [9, 0, 3, 256], [10, 0, 13]],
        nominals=[], interfaces=[], defaults=[])
    body, _ = encoder.encode_body(model, 0)
    assert body[-8:] == bytes(8)
    body = body[:-8] + bytes(4) + body[-8:]
    packet = fields.frame(body, 28, 75)
    fields.header(HERE / 'xir_hash75_golden.h', 'XIR_HASH75_GOLDEN_H', [('hash75_golden', packet)], args.write)
    identities = load('derive_source_product_identity')
    for filename, mapping in identities.frames().items():
        fixed = dict(arrays(HERE / filename))
        for name, expected in mapping.items(): assert fixed[name] == expected, (filename, name)
    current = identities.frames(25, 75)
    rows = [item for mapping in current.values() for item in mapping.items()]
    fields.header(HERE / 'xir_hash75_identity.h', 'XIR_HASH75_IDENTITY_H', rows, args.write)
    print(json.dumps({'status': 'COMPLETE_INDEPENDENT_75_FIELD_MODELS', 'records': records,
                      'hash_model': model, 'hash_packet_bytes': len(packet),
                      'hash_packet_sha256': hashlib.sha256(packet).hexdigest(),
                      'writer_or_executable_used': False}, indent=2))


if __name__ == '__main__': main()
