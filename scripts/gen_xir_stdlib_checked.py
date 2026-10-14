"""Generate verified Checked-only library bytes, immutable data and a component manifest."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def atomic_write(path: Path, data: bytes) -> None:
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
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='xir-stdlib-checked-') as temporary:
        input_root = Path(temporary).resolve()
        (input_root/'io').mkdir()
        shutil.copyfile(root/'stdlib/io/output.xr', input_root/'io/output.xr')
        checked = input_root/'io-output.chk'
        subprocess.run([str(args.producer.resolve()), str(input_root),
                        str(input_root/'io/output.xr'), str(checked)], check=True)
        data = checked.read_bytes()
    if not data:
        raise ValueError('empty Checked library')
    digest = hashlib.sha256(data).digest()
    array = '\n'.join('    ' + ','.join(str(v) for v in data[i:i+24]) + ','
                      for i in range(0, len(data), 24))
    source = ('/* Generated immutable Checked library data; no native code. */\n'
              '#include "xir/xxir_stdlib_internal.h"\n'
              'XR_DATADEF const uint8_t xir_stdlib_output_checked[] = {\n' + array + '\n};\n'
              'XR_DATADEF const size_t xir_stdlib_output_checked_length = ' + str(len(data)) + 'u;\n'
              'XR_DATADEF const uint8_t xir_stdlib_output_checked_sha256[32] = {' +
              ','.join(str(v) for v in digest) + '};\n')
    manifest = dict(schema=1, module_identity='stdlib-module-v1:module=2:io:path=12:io/output.xr',
                    components=[dict(role='checked', path='io-output.chk', length=len(data), sha256=digest.hex())])
    atomic_write(directory/'io-output.chk', data)
    atomic_write(directory/'stdlib-checked-data.c', source.encode('utf-8'))
    atomic_write(directory/'stdlib-manifest.json', (json.dumps(manifest, indent=2)+'\n').encode('utf-8'))


if __name__ == '__main__':
    main()
