"""Bind a genuine Checked/native static pair to its actual compiled object."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from gen_xir_runtime_sdk_recipe import recipe


def word(value: int) -> bytes:
    return struct.pack('<Q', value)


def frame(value: bytes) -> bytes:
    return word(len(value)) + value


def version(root: Path, header: str, name: str) -> int:
    text = (root/'src/xir'/header).read_text(encoding='utf-8')
    match = re.search(r'^#define ' + name + r' (\d+)u?$', text, re.MULTILINE)
    if not match:
        raise ValueError('missing exact ABI declaration: ' + name)
    return int(match[1])


def array(name: str, data: bytes) -> str:
    return 'static const uint8_t ' + name + '[] = {\n' + '\n'.join(
        '    ' + ','.join(str(byte) for byte in data[i:i+24]) + ','
        for i in range(0, len(data), 24)) + '\n};\n'


def digest_initializer(data: bytes) -> str:
    return '{' + ','.join(str(byte) for byte in data) + '}'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--producer', type=Path)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--object', type=Path)
    parser.add_argument('--variant')
    parser.add_argument('--compiler', type=Path)
    args = parser.parse_args()
    root, directory = args.root.resolve(), args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if args.producer:
        with tempfile.TemporaryDirectory(prefix='xir-cache-pair-') as temporary:
            input_root = Path(temporary).resolve()
            (input_root/'io').mkdir()
            shutil.copyfile(root/'stdlib/io/output.xr', input_root/'io/output.xr')
            shutil.copyfile(root/'src/xir/xxir_native_cache_output.xr', input_root/'root.xr')
            argv = [str(args.producer.resolve()), str(input_root), str(input_root/'io/output.xr'),
                    str(input_root/'root.xr'), str(directory/'io-output.chk'),
                    str(directory/'io-output.c'), str(directory/'pair.json')]
            subprocess.run(argv, check=True)
        return
    if not args.object or not args.variant or not args.compiler:
        parser.error('registry needs the actual object, variant and compiler')
    metadata = json.loads((directory/'pair.json').read_text())
    if metadata['schema'] != 1 or len(metadata['leaf_indices']) != 2:
        raise ValueError('unknown pair schema or entry set')
    components = [(directory/'io-output.chk').read_bytes(), args.object.read_bytes(),
                  (directory/'io-output.c').read_bytes()]
    if not all(components):
        raise ValueError('empty pair component')
    rows = recipe(root)['files']
    sdk_hash = hashlib.sha256(b'xir-same-source-sdk-v1')
    files = []
    for row in rows:
        if row['kind'] == 5:
            continue
        path = root/row['path']
        data = path.read_bytes()
        digest = hashlib.sha256(data).digest()
        sdk_hash.update(frame(row['path'].encode()) + word(row['kind']) + word(len(data)) + digest)
        files.append(dict(**row, length=len(data), sha256=digest.hex()))
    sdk = sdk_hash.digest()
    compiler = args.compiler.resolve()
    compiler_digest = hashlib.sha256(compiler.read_bytes()).hexdigest()
    variant = (args.variant + '|compiler-sha256=' + compiler_digest).encode()
    hashes = [hashlib.sha256(data).digest() for data in components]
    semantic = bytes.fromhex(metadata['semantic_id'])
    abi = bytes.fromhex(metadata['runtime_layout'])
    leaves = [bytes.fromhex(value) for value in metadata['leaf_digests']]
    indices = metadata['leaf_indices']
    versions = [version(root, header, name) for header, name in (
        ('xxir_value.h', 'XR_XIR_VALUE_ABI_VERSION'),
        ('xxir_call.h', 'XR_XIR_CALL_ABI_VERSION'),
        ('xxir_program.h', 'XR_XIR_PROGRAM_ABI_VERSION'))]
    binding = b''.join(word(len(data))+digest for data, digest in zip(components, hashes))
    binding += b''.join(word(value) for value in (1, 1, *versions, metadata['entry_count']))
    binding += semantic + abi + sdk + frame(variant)
    binding += b''.join(word(index)+digest for index, digest in zip(indices, leaves))
    binding_digest = hashlib.sha256(binding).digest()
    output = ['/* Generated from actual Checked, C, native object and same-source inputs. */',
              '#include "xir/xxir_native_cache_registry_internal.h"',
              'extern const XrXirCallEntry xir_output_cache_entries[];']
    for name, data in zip(('cache_checked', 'cache_object', 'cache_c'), components):
        output.append(array(name, data))
    output.append('const XirNativeCacheRegistry xir_native_cache_registry = {1u,')
    for name, data, digest in zip(('cache_checked', 'cache_object', 'cache_c'), components, hashes):
        output.append('{' + name + ',' + str(len(data)) + 'u,' + digest_initializer(digest) + '},')
    output.append(','.join(digest_initializer(digest) for digest in (semantic, abi, sdk, binding_digest)) + ',')
    output.append(json.dumps(variant.decode()) + ',1u,' + ','.join(str(value)+'u' for value in versions) + ',')
    output.append('{' + ','.join(str(index)+'u' for index in indices) + '},')
    output.append('{' + ','.join(digest_initializer(digest) for digest in leaves) + '},')
    output.append('xir_output_cache_entries,' + str(metadata['entry_count']) + 'u};\n')
    (directory/'native-cache-registry.c').write_text('\n'.join(output), encoding='utf-8', newline='\n')
    native = directory/'io-output.native'
    with tempfile.NamedTemporaryFile(dir=directory, delete=False) as pending:
        pending.write(components[1])
        pending_native = Path(pending.name)
    os.replace(pending_native, native)
    roles = ('checked', 'native-object', 'generated-c')
    paths = ('io-output.chk', 'io-output.native', 'io-output.c')
    record = dict(metadata, components=[dict(role=role, path=path, length=len(data), sha256=digest.hex())
                                       for role, path, data, digest in zip(roles, paths, components, hashes)],
                  variant=variant.decode(), sdk_identity=sdk.hex(), sdk_files=files,
                  binding_digest=binding_digest.hex(), schema=2,
                  module_identity='stdlib-module-v1:module=2:io:path=12:io/output.xr',
                  value_abi=versions[0], call_abi=versions[1], program_abi=versions[2])
    with tempfile.NamedTemporaryFile(dir=directory, mode='w', encoding='utf-8', newline='\n', delete=False) as pending:
        pending.write(json.dumps(record, indent=2)+'\n')
        pending_manifest = Path(pending.name)
    os.replace(pending_manifest, directory/'registry-manifest.json')


if __name__ == '__main__':
    main()
