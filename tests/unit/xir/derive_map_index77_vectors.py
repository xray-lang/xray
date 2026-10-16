"""Independent required-Map-lookup packet and current framing; retain contract76."""
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
    source = args.source.resolve()
    folder = source / 'tests/unit/xir'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(folder))
    import derive_hash76_vectors as previous
    utility = previous.load(folder / 'derive_construction72_vectors.py', 'index_frames77')
    saved = sys.argv
    try:
        sys.argv = ['retained76', '--source', str(source), '--output', str(output / 'retained76')]
        with contextlib.redirect_stdout(io.StringIO()): previous.main()
    finally: sys.argv = saved
    records = []
    historical = previous.load(folder / 'derive_hash75_vectors.py', 'index_arrays')
    for path in sorted((output / 'retained76').glob('*76*golden*.h')):
        rows = []
        for name, data in historical.arrays(path):
            if data.startswith(b'XRCHK\0\0\0'):
                data = utility.frame(data[64:], 28, 77)
                rows.append((name.replace('76', '77'), data))
                records.append(dict(symbol=rows[-1][0], bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
                if name == 'checked_scalar76_golden': rows.append(('checked_scalar77_digest', data[32:64]))
            elif name.endswith('digest') and '76' in name: continue
            else: rows.append((name, data))
        macros = [(name.replace('76', '77'), int(value)) for name, value in
            re.findall(r'#define (\w*76\w*) (\d+)u?', path.read_text(encoding='utf-8'))]
        target = output / path.name.replace('76', '77')
        utility.header(target, target.stem.upper() + '_H', rows, True, macros)
        destination = folder / ('compile_owner' if path.name.startswith('checked_sizing') else '') / target.name
        utility.header(destination, target.stem.upper() + '_H', rows, args.write, macros)
    identities = previous.load(folder / 'derive_source_product_identity.py', 'index_identity77')
    rows = [item for mapping in identities.frames(25, 77).values() for item in mapping.items()]
    for semantic in (75, 76):
        rows += [(name.replace('_policy_value25', '_policy' + str(semantic)), data)
            for mapping in identities.frames(25, semantic).values() for name, data in mapping.items()
            if '_policy_value25' in name]
    utility.header(output / 'xir_hash77_identity.h', 'XIR_HASH77_IDENTITY_H', rows, True)
    utility.header(folder / 'xir_hash77_identity.h', 'XIR_HASH77_IDENTITY_H', rows, args.write)
    # Every instruction and type field is independent of the product writer.
    model = dict(linkage=0, functions=[dict(name='index', parameters=[], result=2,
        blocks=[[0, 7, 0, 0]], instructions=[
            [154, 256, 0, 0, 0, 0, 0, 0, 0], [34, 256, 0, 0, 0, 0, 0, 0, 0],
            [2, 2, 0, 0, 0, 0, 7, 0, 0], [2, 2, 0, 0, 0, 0, 19, 0, 0],
            [159, 0, 0, 3, 0, 0, 0, 0, 0], [163, 2, 1, 2, 0, 0, 0, 0, 0],
            [33, 0, 5, 0, 0, 0, 0, 0, 0]], operands=[1, 2, 3])],
        declarations=None, generics=None, nodes=[[9, 0, 2, 2]], nominals=[], interfaces=[], defaults=[])
    encoder = previous.load(folder / 'derive_callable71_checked_vectors.py', 'index_encoder77')
    body, _ = encoder.encode_body(model, 0)
    assert body[-8:] == bytes(8)
    body = body[:-8] + bytes(4) + body[-8:]
    data = utility.frame(body, 28, 77)
    rows = [('map_index77_golden', data)]
    for name in ('result', 'key'):
        invalid = copy.deepcopy(model)
        function = invalid['functions'][0]
        if name == 'result':
            function['result'] = 3
            function['instructions'][5][1] = 3
        else:
            function['instructions'][5][3] = 0
        encoded, _ = encoder.encode_body(invalid, 0)
        encoded = encoded[:-8] + bytes(4) + encoded[-8:]
        rows.append(('map_index77_bad_' + name, utility.frame(encoded, 28, 77)))
    utility.header(output / 'xir_map_index77_golden.h', 'XIR_MAP_INDEX77_GOLDEN_H', rows, True)
    utility.header(folder / 'xir_map_index77_golden.h', 'XIR_MAP_INDEX77_GOLDEN_H', rows, args.write)
    records.append(dict(symbol='map_index77_golden', bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), model=model))
    report = dict(previous76_complete_field_models='VERIFIED', records=records, product_writer_used=False)
    (output / 'vectors77.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(records=len(records), required_lookup=records[-1], product_writer_used=False)))

if __name__ == '__main__': main()
