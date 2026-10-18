"""Bind public CLI executions to one immutable, complete product file closure."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError('duplicate product manifest member: ' + name)
        result[name] = value
    return result


def _relative(name: str) -> str:
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name
            or '\0' in name or name.startswith('/') or name.endswith('/')
            or any(part in ('', '.', '..') or part.endswith((' ', '.'))
                   for part in name.split('/'))
            or str(PurePosixPath(name)) != name):
        raise ValueError('product file path must be canonical and relative')
    return name


def _linked(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


class FixedProductInputs:
    def __init__(self, manifest_path: Path, binary: Path):
        if _linked(manifest_path):
            raise ValueError('product manifest must not be a link')
        self.manifest_path = manifest_path.resolve(strict=True)
        self.root = self.manifest_path.parent
        self.raw = self.manifest_path.read_bytes()
        self.manifest = json.loads(self.raw, object_pairs_hook=_object)
        data = self.manifest
        if type(data.get('schema')) is not int or data['schema'] != 1:
            raise ValueError('unsupported product inputs schema')
        if not re.fullmatch('[0-9a-f]{40}', data.get('source_commit', '')):
            raise ValueError('product inputs require an exact source commit')
        self.files = data.get('files')
        if not isinstance(self.files, dict) or not self.files:
            raise ValueError('product inputs require a complete file table')
        aliases = {self.manifest_path.name.casefold()}
        for name, value in self.files.items():
            _relative(name)
            if name.casefold() in aliases or not isinstance(value, str) or not re.fullmatch('[0-9a-f]{64}', value):
                raise ValueError('product file aliases or digests are invalid')
            aliases.add(name.casefold())
        self.binary = self.root / _relative(data['binary'])
        self.stdlib = self.root / _relative(data['stdlib_root'])
        if (data['binary'] not in self.files
                or data.get('binary_sha256') != self.files[data['binary']]
                or not any(name.startswith(data['stdlib_root'] + '/') for name in self.files)):
            raise ValueError('product binary or standard library is unbound')
        if _linked(binary) or binary.resolve(strict=True) != self.binary.resolve(strict=True):
            raise ValueError('CLI argument is not the bound public binary')
        version = data.get('version', {})
        if (version.get('product') != 'xray-lang' or version.get('commit') != data['source_commit']
                or version.get('dirty') is not False or not version.get('target')
                or not {'vm', 'aot'} <= set(version.get('features', []))):
            raise ValueError('product inputs require a clean public CLI version')
        self.verify()

    def verify(self) -> None:
        if _linked(self.manifest_path) or self.manifest_path.read_bytes() != self.raw:
            raise ValueError('product inputs manifest changed during execution')
        observed = set()
        for directory, folders, files in os.walk(self.root, followlinks=False):
            for name in folders + files:
                path = Path(directory) / name
                if _linked(path):
                    raise ValueError('product file closure must not contain links')
            for name in files:
                path = Path(directory) / name
                relative = path.relative_to(self.root).as_posix()
                if path == self.manifest_path:
                    continue
                observed.add(relative)
                if (not path.resolve(strict=True).is_relative_to(self.root)
                        or relative not in self.files or _sha(path.read_bytes()) != self.files[relative]):
                    raise ValueError('product file changed or is unlisted: ' + relative)
        if observed != set(self.files) or not self.stdlib.is_dir():
            raise ValueError('product file closure is incomplete')

    def require_version(self, version: dict) -> None:
        if version != self.manifest['version']:
            raise ValueError('actual CLI version differs from the fixed product inputs')

    def evidence(self) -> dict:
        return {'manifest': str(self.manifest_path), 'manifest_sha256': _sha(self.raw),
                'source_commit': self.manifest['source_commit'], 'file_count': len(self.files),
                'binary_sha256': self.files[self.manifest['binary']], 'stdlib': str(self.stdlib),
                'protocol_record': self.manifest.get('protocol'),
                'scope': 'EXACT_SUPPLIED_INPUT_BINDING_NOT_FULL_PRODUCT_QUALIFICATION'}
