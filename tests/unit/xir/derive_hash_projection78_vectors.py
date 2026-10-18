"""Independent ordered-projection packets; reproduce and retain contract77."""
import argparse
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import re
import sys
sys.dont_write_bytecode = True

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    folder = source / 'tests/unit/xir'
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(folder))
    import derive_map_index77_vectors as previous
    import derive_hash76_vectors as loaders
    utility = loaders.load(folder / 'derive_construction72_vectors.py', 'projection_frames78')
    historical = loaders.load(folder / 'derive_hash75_vectors.py', 'projection_arrays78')
    saved = sys.argv
    try:
        sys.argv = ['retained77', '--source', str(source), '--output', str(output / 'retained77')]
        with contextlib.redirect_stdout(io.StringIO()): previous.main()
    finally: sys.argv = saved
    records = []
    for path in sorted((output / 'retained77').glob('*77*golden*.h')):
        rows = []
        for name, data in historical.arrays(path):
            if data.startswith(b'XRCHK\0\0\0'):
                data = utility.frame(data[64:], 28, 78)
                rows.append((name.replace('77', '78'), data))
                records.append(dict(symbol=rows[-1][0], bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
                if name == 'checked_scalar77_golden': rows.append(('checked_scalar78_digest', data[32:64]))
            elif name.endswith('digest') and '77' in name: continue
            else: rows.append((name, data))
        macros = [(name.replace('77', '78'), int(value)) for name, value in
            re.findall(r'#define (\w*77\w*) (\d+)u?', path.read_text(encoding='utf-8'))]
        target = output / path.name.replace('77', '78')
        utility.header(target, target.stem.upper() + '_H', rows, True, macros)
        destination = folder / ('compile_owner' if path.name.startswith('checked_sizing') else '') / target.name
        utility.header(destination, target.stem.upper() + '_H', rows, args.write, macros)
    identities = loaders.load(folder / 'derive_source_product_identity.py', 'projection_identity78')
    rows = [item for mapping in identities.frames(25, 78).values() for item in mapping.items()]
    for semantic in (75, 76, 77):
        rows += [(name.replace('_policy_value25', '_policy' + str(semantic)), data)
            for mapping in identities.frames(25, semantic).values() for name, data in mapping.items()
            if '_policy_value25' in name]
    utility.header(output / 'xir_hash78_identity.h', 'XIR_HASH78_IDENTITY_H', rows, True)
    utility.header(folder / 'xir_hash78_identity.h', 'XIR_HASH78_IDENTITY_H', rows, args.write)
    encoder = loaders.load(folder / 'derive_callable71_checked_vectors.py', 'projection_encoder78')
    def model(op, container, element, entries=False):
        nodes = [container, [2, 0, element]]
        if entries: nodes = [container, [6, 0, [2, 3]], [2, 0, 257]]
        result = 258 if entries else 257
        return dict(linkage=0, functions=[dict(name='project', parameters=[256], result=result,
            blocks=[[0, 2, 0, 0]], instructions=[
                [op, result, 0, 0, 0, 0, 0, 0, 0], [33, 0, 1, 0, 0, 0, 0, 0, 0]], operands=[])],
            declarations=None, generics=None, nodes=nodes, nominals=[], interfaces=[], defaults=[])
    models = {
        'keys': model(164, [9, 0, 2, 3], 2),
        'values': model(165, [9, 0, 2, 3], 3),
        'entries': model(166, [9, 0, 2, 3], 0, True),
        'set_values': model(165, [10, 0, 2], 2)}
    models['bad_keys'] = model(164, [9, 0, 2, 3], 3)
    models['bad_values'] = model(165, [9, 0, 2, 3], 2)
    models['bad_set_keys'] = model(164, [10, 0, 2], 2)
    models['bad_entries'] = model(166, [9, 0, 2, 3], 3)
    models['bad_entry_fields'] = copy.deepcopy(models['entries'])
    models['bad_entry_fields']['nodes'][1][2] = [3, 2]
    rows = []
    for name, description in models.items():
        body, _ = encoder.encode_body(description, 0)
        assert body[-8:] == bytes(8)
        data = utility.frame(body[:-8] + bytes(4) + body[-8:], 28, 78)
        rows.append(('hash_projection78_' + name, data))
        records.append(dict(symbol=rows[-1][0], bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), model=description))
    utility.header(output / 'xir_hash_projection78_golden.h', 'XIR_HASH_PROJECTION78_GOLDEN_H', rows, True)
    utility.header(folder / 'xir_hash_projection78_golden.h', 'XIR_HASH_PROJECTION78_GOLDEN_H', rows, args.write)
    report = dict(previous77_complete_field_models='VERIFIED', records=records, product_writer_used=False)
    (output / 'vectors78.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(records=len(records), new_packets=9, product_writer_used=False)))

if __name__ == '__main__': main()
