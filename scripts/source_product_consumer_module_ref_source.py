#!/usr/bin/env python3
"""Preserve legal module-ref expectations and independent local-cell controls."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers/module_ref_source'
FOOT = 'export fn consumerAnswer() -> i64 { return answer() }\nfn main() -> i64 { return 0 }\n'
SCALAR = 'fn bump(value: ref i64) { value += 1 }\nfn relay(value: ref i64) { bump(ref value) }\n'
ARRAY = 'fn bump(value: ref Array<i64>) { value[0] = value[0] + 1 }\nfn relay(value: ref Array<i64>) { bump(ref value) }\n'


def cases():
    def row(name, body, expected=0, library=None):
        files = {'root.xr': body + FOOT}
        if library is not None: files['library.xr'] = library
        return dict(name=name, expected=expected, files=files)
    return [
        row('local_scalar', SCALAR + 'fn answer() -> i64 { var value = 40; relay(ref value); bump(ref value); return value }\n'),
        row('local_array', ARRAY + 'fn answer() -> i64 { const original = [40]; var value = original; relay(ref value); bump(ref value); assert(original[0] == 40); return value[0] }\n'),
        row('root_scalar', SCALAR + 'var value = 40\nfn answer() -> i64 { relay(ref value); bump(ref value); return value }\n'),
        row('root_array', ARRAY + 'var value = [40]\nfn answer() -> i64 { relay(ref value); bump(ref value); return value[0] }\n'),
        row('imported_scalar', 'import { next } from "./library"\nfn answer() -> i64 { return next() }\n',
            library=SCALAR + 'var value = 40\nexport fn next() -> i64 { relay(ref value); bump(ref value); return value }\n'),
        row('root_const', SCALAR + 'const value = 40\nfn answer() -> i64 { relay(ref value); return value }\n', 3),
        row('local_const', SCALAR + 'fn answer() -> i64 { const value = 40; relay(ref value); return value }\n', 3),
        row('local_missing_ref', SCALAR + 'fn answer() -> i64 { var value = 40; relay(value); return value }\n', 3),
        row('local_alias', 'fn pair(a: ref i64, b: ref i64) { a += 1; b += 1 }\nfn answer() -> i64 { var value = 40; pair(ref value, ref value); return value }\n', 3),
    ]


def artifacts():
    result = {}
    manifest = dict(schema=1, cases=[])
    lines = ['/* Positive module-ref source remains a positive obligation. */',
             'typedef struct ModuleRefSourceCase { const char *name; XrXirStatus expected; } ModuleRefSourceCase;',
             'static const ModuleRefSourceCase module_ref_source_cases[] = {']
    for case in cases():
        hashes = {}
        for filename, text in case['files'].items():
            result[ROOT/case['name']/filename] = text
            hashes[filename] = hashlib.sha256(text.encode()).hexdigest()
        manifest['cases'].append(dict(name=case['name'], expected=case['expected'], files=hashes,
                                     runtime_expected=42 if not case['expected'] else None))
        lines.append('    {"%s", %u},' % (case['name'], case['expected']))
    lines += ['};', '']
    manifest['boundary'] = 'New Source witnesses of old ref duties; not literal extraction of old CoreIR fixtures. Module positives are not converted into rejects. Local Cell controls are separate and do not retire module/ref/interface obligations.'
    result[ROOT/'cases.h'] = '\n'.join(lines)
    result[ROOT/'manifest.json'] = json.dumps(manifest, indent=2) + '\n'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write', 'check'))
    args = parser.parse_args()
    for path, text in artifacts().items():
        if args.mode == 'write':
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf8', newline='\n')
        elif path.read_text(encoding='utf8') != text: raise ValueError(f'module-ref input differs: {path}')
    print('Module ref source:5 positive obligations,4 exact type rejects;2 local runtime controls expect42')
