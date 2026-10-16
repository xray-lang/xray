"""Independent binary Set fields; reproduce and retain the complete78 models."""
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
    import derive_hash_projection78_vectors as previous
    import derive_hash76_vectors as loaders
    utility = loaders.load(folder / 'derive_construction72_vectors.py', 'set_frames79')
    historical = loaders.load(folder / 'derive_hash75_vectors.py', 'set_arrays79')
    saved = sys.argv
    try:
        sys.argv = ['retained78', '--source', str(source), '--output', str(output / 'retained78')]
        with contextlib.redirect_stdout(io.StringIO()): previous.main()
    finally: sys.argv = saved
    records = []
    for path in sorted((output / 'retained78').glob('*78*golden*.h')):
        rows = []
        for name, data in historical.arrays(path):
            if data.startswith(b'XRCHK\0\0\0'):
                data = utility.frame(data[64:], 28, 79)
                rows.append((name.replace('78', '79'), data))
                records.append(dict(symbol=rows[-1][0], bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
                if name == 'checked_scalar78_golden': rows.append(('checked_scalar79_digest', data[32:64]))
            elif name.endswith('digest') and '78' in name: continue
            else: rows.append((name, data))
        macros = [(name.replace('78', '79'), int(value)) for name, value in
            re.findall(r'#define (\w*78\w*) (\d+)u?', path.read_text(encoding='utf-8'))]
        target = output / path.name.replace('78', '79')
        utility.header(target, target.stem.upper() + '_H', rows, True, macros)
        destination = folder / ('compile_owner' if path.name.startswith('checked_sizing') else '') / target.name
        utility.header(destination, target.stem.upper() + '_H', rows, args.write, macros)
    identities = loaders.load(folder / 'derive_source_product_identity.py', 'set_identity79')
    rows = [item for mapping in identities.frames(25, 79).values() for item in mapping.items()]
    for semantic in (75, 76, 77, 78):
        rows += [(name.replace('_policy_value25', '_policy' + str(semantic)), data)
            for mapping in identities.frames(25, semantic).values() for name, data in mapping.items()
            if '_policy_value25' in name]
    utility.header(output / 'xir_hash79_identity.h', 'XIR_HASH79_IDENTITY_H', rows, True)
    utility.header(folder / 'xir_hash79_identity.h', 'XIR_HASH79_IDENTITY_H', rows, args.write)
    encoder = loaders.load(folder / 'derive_callable71_checked_vectors.py', 'set_encoder79')
    def model(operation):
        result = 1 if operation >= 171 else 256
        return dict(linkage=0, functions=[dict(name='algebra', parameters=[256,256], result=result,
            blocks=[[0,2,0,0]], instructions=[[operation,result,0,1,0,0,0,0,0],[33,0,2,0,0,0,0,0,0]], operands=[])],
            declarations=None, generics=None, nodes=[[10,0,2]], nominals=[], interfaces=[], defaults=[])
    models = {name:model(operation) for name,operation in zip(
        ('union','intersection','difference','symmetric_difference','is_subset','is_superset'), range(167,173))}
    models['bad_receiver'] = copy.deepcopy(models['union']); models['bad_receiver']['nodes'][0] = [9,0,2,2]
    models['bad_other_kind'] = copy.deepcopy(models['union'])
    models['bad_other_kind']['nodes'].append([9,0,2,2]); models['bad_other_kind']['functions'][0]['parameters'][1] = 257
    models['bad_other_element'] = copy.deepcopy(models['union'])
    models['bad_other_element']['nodes'].append([10,0,5]); models['bad_other_element']['functions'][0]['parameters'][1] = 257
    for name, base, result in [('bad_set_result','union',1),('bad_relation_result','is_subset',256)]:
        models[name] = copy.deepcopy(models[base]); models[name]['functions'][0]['result'] = result
        models[name]['functions'][0]['instructions'][0][1] = result
    models['bad_hashable'] = copy.deepcopy(models['union']); models['bad_hashable']['nodes'] = [[10,0,257],[2,0,2]]
    rows = []
    for name, description in models.items():
        body, _ = encoder.encode_body(description, 0)
        assert body[-8:] == bytes(8)
        data = utility.frame(body[:-8] + bytes(4) + body[-8:], 28, 79)
        rows.append(('set_algebra79_' + name, data))
        records.append(dict(symbol=rows[-1][0], bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), model=description))
    utility.header(output / 'xir_set_algebra79_golden.h', 'XIR_SET_ALGEBRA79_GOLDEN_H', rows, True)
    utility.header(folder / 'xir_set_algebra79_golden.h', 'XIR_SET_ALGEBRA79_GOLDEN_H', rows, args.write)
    (output / 'vectors79.json').write_text(json.dumps(dict(previous78_complete_field_models='VERIFIED',
        records=records, product_writer_used=False), indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(records=len(records), new_packets=12, product_writer_used=False)))

if __name__ == '__main__': main()
