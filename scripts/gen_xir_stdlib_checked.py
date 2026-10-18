"""Generate verified Checked-only libraries, immutable data and exact publications."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

LIBRARIES = (
    ('output', 'io', 'io/output.xr', 'io/output/io-output.chk'),
    ('time', 'time', 'time/time.xr', 'time/time.chk'),
    ('math', 'math', 'math/math.xr', 'math/math.chk'),
)


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pending_path = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as pending:
            pending.write(data)
            pending_path = Path(pending.name)
        os.replace(pending_path, path)
    finally:
        if pending_path is not None and pending_path.exists():
            pending_path.unlink()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--producer', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    root, directory = args.root.resolve(), args.directory.resolve()
    source = ['/* Generated immutable Checked library data; no native code. */',
              '#include "xir/xxir_stdlib_internal.h"']
    components, rows, packets = [], [], []
    with tempfile.TemporaryDirectory(prefix='xir-stdlib-checked-') as temporary:
        input_root = Path(temporary).resolve()
        for symbol, namespace, logical, published in LIBRARIES:
            entry = input_root / logical
            entry.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / 'stdlib' / logical, entry)
            checked = input_root / (symbol + '.chk')
            subprocess.run([str(args.producer.resolve()), str(input_root), str(entry),
                            namespace, str(checked)], check=True)
            data = checked.read_bytes()
            if not data:
                raise ValueError('empty Checked library: ' + logical)
            digest = hashlib.sha256(data).digest()
            prefix = 'xir_stdlib_' + symbol + '_checked'
            visibility = 'XR_DATADEF' if symbol == 'output' else 'static'
            source.append(visibility + ' const uint8_t ' + prefix + '[] = {')
            source.extend('    ' + ','.join(str(v) for v in data[i:i+24]) + ','
                          for i in range(0, len(data), 24))
            source.extend(['};', visibility + ' const uint8_t ' + prefix + '_sha256[32] = {' +
                           ','.join(str(v) for v in digest) + '};'])
            if symbol == 'output':
                source.append('XR_DATADEF const size_t ' + prefix + '_length = ' + str(len(data)) + 'u;')
            rows.append('    {' + prefix + ',sizeof(' + prefix + '),' + prefix + '_sha256,"' +
                        namespace + '","' + logical + '"},')
            identity = 'stdlib-module-v1:module=' + str(len(namespace)) + ':' + namespace + ':path=' + str(len(logical)) + ':' + logical
            components.append(dict(role='checked', path=published, module_identity=identity,
                                   length=len(data), sha256=digest.hex()))
            packets.append((published, data))
    source.extend(['XR_DATADEF const XirStdlibCheckedComponent xir_stdlib_checked_components[] = {',
                   *rows, '};', 'XR_DATADEF const size_t xir_stdlib_checked_component_count = ' + str(len(rows)) + 'u;', ''])
    # The complete set is built before any publication is replaced.
    for name, data in packets:
        atomic_write(directory / name, data)
    atomic_write(directory / 'stdlib-checked-data.c', '\n'.join(source).encode('utf-8'))
    manifest = dict(schema=2, components=components)
    atomic_write(directory / 'stdlib-manifest.json', (json.dumps(manifest, indent=2)+'\n').encode('utf-8'))


if __name__ == '__main__':
    main()
