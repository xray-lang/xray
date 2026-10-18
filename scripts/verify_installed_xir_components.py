"""Verify relocatable Checked library and independent SDK publication against trusted build records."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path, PurePosixPath
from build_xir_runtime_sdk import canonical_identity

FIELDS = {'schema', 'components'}


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
    rows = record['components']
    if not isinstance(rows, list) or not rows:
        raise ValueError('empty or invalid component inventory')
    identities, paths = set(), set()
    for row in rows:
        if set(row) != {'role', 'path', 'module_identity', 'length', 'sha256'} or row['role'] != 'checked':
            raise ValueError('unknown component fields or role')
        identity = row['module_identity']
        match = re.fullmatch(r'stdlib-module-v1:module=([1-9][0-9]*):([A-Za-z_][A-Za-z_0-9]*):path=([1-9][0-9]*):([A-Za-z_][A-Za-z_0-9]*(?:/[A-Za-z_][A-Za-z_0-9]*)+\.xr)', identity)
        if not match or int(match[1]) != len(match[2]) or int(match[3]) != len(match[4]) or not match[4].startswith(match[2]+'/'):
            raise ValueError('wrong stdlib module identity')
        if identity in identities or row['path'] in paths:
            raise ValueError('duplicate component identity or path')
        identities.add(identity)
        paths.add(row['path'])
        file_matches(root, row)
    sdk_identity = None
    if sdk_root is not None and (sdk_root.exists() or sdk_root.is_symlink()):
        sdk_identity = verify_sdk(sdk_root, expected_sdk)
    return raw, sdk_identity


def verify_sdk(root, expected):
    if root.is_symlink() or not root.is_dir() or expected is None:
        raise ValueError('incomplete SDK root or missing trusted record')
    if regular(root, 'sdk_manifest.json').read_bytes() != expected.read_bytes():
        raise ValueError('installed SDK manifest differs from trusted build')
    sdk = json.loads(expected.read_bytes())
    if type(sdk['schema']) is not int or sdk['schema'] != 2:
        raise ValueError('unknown SDK schema')
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
        publication = args.directory/'stdlib-manifest.json'
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
        elif regular(args.directory, 'stdlib-manifest.json').read_bytes() != raw:
            raise ValueError('publication differs from trusted build')
        print(json.dumps({'status': 'VERIFIED_CHECKED_STDLIB', 'sdk_identity': sdk_identity,
                          'native_sdk_present': sdk_identity is not None}))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('XIR component verification failed: ' + str(error), file=__import__('sys').stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
