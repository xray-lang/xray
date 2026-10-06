"""Verify relocatable static-pair publication against trusted build records."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path, PurePosixPath
from build_xir_runtime_sdk import canonical_identity
from gen_xir_native_cache_pair import frame, word

COMPONENTS = (('checked', 'io-output.chk'), ('native-object', 'io-output.native'),
              ('generated-c', 'io-output.c'))
FIELDS = {'schema', 'semantic_id', 'runtime_layout', 'entry_count', 'leaf_indices',
          'leaf_digests', 'components', 'variant', 'sdk_identity', 'sdk_files',
          'binding_digest', 'module_identity', 'value_abi', 'call_abi', 'program_abi'}


def digest(value):
    if not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
        raise ValueError('invalid digest')
    return bytes.fromhex(value)


def regular(root, relative):
    if not isinstance(relative, str) or not relative or '\\' in relative or ':' in relative or '\0' in relative:
        raise ValueError('invalid relative path')
    path = PurePosixPath(relative)
    if path.is_absolute() or path.as_posix() != relative or any(p in ('.', '..') for p in path.parts):
        raise ValueError('noncanonical relative path')
    file = root.joinpath(*path.parts)
    if file.is_symlink() or not file.is_file() or not file.resolve().is_relative_to(root.resolve()):
        raise ValueError('missing or escaped regular file: ' + relative)
    return file


def file_matches(root, row):
    data = regular(root, row['path']).read_bytes()
    if type(row['length']) is not int or row['length'] <= 0 or len(data) != row['length']:
        raise ValueError('component length mismatch: ' + row['path'])
    if hashlib.sha256(data).digest() != digest(row['sha256']):
        raise ValueError('component digest mismatch: ' + row['path'])


def verify_components(root, expected, sdk_root, expected_sdk):
    raw = expected.read_bytes()
    record = json.loads(raw)
    if set(record) != FIELDS or type(record['schema']) is not int or record['schema'] != 2:
        raise ValueError('unknown publication schema or fields')
    if record['module_identity'] != 'stdlib-module-v1:module=2:io:path=12:io/output.xr':
        raise ValueError('wrong stdlib module identity')
    if [(r.get('role'), r.get('path')) for r in record['components']] != list(COMPONENTS):
        raise ValueError('unknown component role or path')
    for row in record['components']:
        if set(row) != {'role', 'path', 'length', 'sha256'}:
            raise ValueError('unknown component fields')
        file_matches(root, row)
    if type(record['entry_count']) is not int or record['entry_count'] <= 0:
        raise ValueError('invalid entry count')
    indices, leaves = record['leaf_indices'], record['leaf_digests']
    if len(indices) != 2 or len(leaves) != 2 or len(set(indices)) != 2 or any(
            type(i) is not int or i < 0 or i >= record['entry_count'] for i in indices):
        raise ValueError('invalid leaf entry set')
    versions = [record[k] for k in ('value_abi', 'call_abi', 'program_abi')]
    if any(type(v) is not int or v <= 0 for v in versions):
        raise ValueError('invalid ABI')
    if not isinstance(record['variant'], str) or not record['variant'].startswith('triple='):
        raise ValueError('invalid variant')
    sdk_hash = hashlib.sha256(b'xir-same-source-sdk-v1')
    previous = ''
    for row in record['sdk_files']:
        if set(row) != {'path', 'kind', 'length', 'sha256'} or row['path'] <= previous:
            raise ValueError('noncanonical SDK source table')
        previous = row['path']
        path = PurePosixPath(row['path'])
        if path.is_absolute() or path.as_posix() != row['path'] or any(p in ('.', '..') for p in path.parts) or '\\' in row['path'] or ':' in row['path']:
            raise ValueError('invalid SDK source path')
        if type(row['kind']) is not int or not 1 <= row['kind'] <= 4 or type(row['length']) is not int or row['length'] <= 0:
            raise ValueError('invalid SDK source shape')
        sdk_hash.update(frame(row['path'].encode()) + word(row['kind']) + word(row['length']) + digest(row['sha256']))
    if not record['sdk_files'] or sdk_hash.digest() != digest(record['sdk_identity']):
        raise ValueError('SDK source identity mismatch')
    binding = b''.join(word(r['length']) + digest(r['sha256']) for r in record['components'])
    binding += b''.join(word(v) for v in (1, 1, *versions, record['entry_count']))
    binding += digest(record['semantic_id']) + digest(record['runtime_layout']) + sdk_hash.digest() + frame(record['variant'].encode())
    binding += b''.join(word(i) + digest(d) for i, d in zip(indices, leaves))
    if hashlib.sha256(binding).digest() != digest(record['binding_digest']):
        raise ValueError('registry binding mismatch')
    sdk_identity = None
    if sdk_root is not None and (sdk_root.exists() or sdk_root.is_symlink()):
        sdk_identity = verify_sdk(sdk_root, expected_sdk, record)
    return raw, sdk_identity


def verify_sdk(root, expected, pair=None):
    if root.is_symlink() or not root.is_dir() or expected is None:
        raise ValueError('incomplete SDK root or missing trusted record')
    if regular(root, 'sdk_manifest.json').read_bytes() != expected.read_bytes():
        raise ValueError('installed SDK manifest differs from trusted build')
    sdk = json.loads(expected.read_bytes())
    if type(sdk['schema']) is not int or sdk['schema'] != 2:
        raise ValueError('unknown SDK schema')
    if pair is not None:
        if any(sdk[k] != pair[k] for k in ('value_abi', 'call_abi', 'program_abi')):
            raise ValueError('SDK ABI mismatch')
        if not pair['variant'].startswith('triple=' + sdk['target_triple'] + '|'):
            raise ValueError('SDK target mismatch')
        if [r for r in sdk['files'] if r['kind'] != 5] != pair['sdk_files']:
            raise ValueError('pair and SDK source tables differ')
    for row in sdk['files']:
        file_matches(root, row)
    return canonical_identity(sdk).hex()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--expected', type=Path, required=True)
    parser.add_argument('--sdk-root', type=Path)
    parser.add_argument('--expected-sdk', type=Path)
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--sdk-only', action='store_true')
    args = parser.parse_args()
    try:
        if args.sdk_only:
            if args.publish or args.sdk_root is None:
                raise ValueError('invalid SDK-only verification request')
            identity = verify_sdk(args.sdk_root, args.expected_sdk)
            print(json.dumps({'status': 'VERIFIED_RUNTIME_SDK', 'sdk_identity': identity,
                              'native_sdk_present': True}))
            return 0
        raw, sdk_identity = verify_components(args.directory, args.expected, args.sdk_root, args.expected_sdk)
        publication = args.directory/'registry-manifest.json'
        if args.publish:
            pending_path = None
            try:
                with tempfile.NamedTemporaryFile(dir=args.directory, delete=False) as pending:
                    pending.write(raw)
                    pending_path = Path(pending.name)
                os.replace(pending_path, publication)
            finally:
                if pending_path is not None and pending_path.exists():
                    pending_path.unlink()
        elif regular(args.directory, 'registry-manifest.json').read_bytes() != raw:
            raise ValueError('publication differs from trusted build')
        print(json.dumps({'status': 'VERIFIED_STATIC_PAIR', 'sdk_identity': sdk_identity,
                          'native_sdk_present': sdk_identity is not None}))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('stdlib pair verification failed: ' + str(error), file=__import__('sys').stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
