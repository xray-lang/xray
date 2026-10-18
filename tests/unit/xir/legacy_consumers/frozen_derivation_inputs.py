"""Verify immutable derivation evidence without substituting product inputs."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import re

HERE = Path(__file__).resolve().parent
COHORT = '8bbf31427772e0a9245a5126144ce421cd6b8e86'


def resolve(root: Path, relative: str, expected_lf_sha256: str) -> Path:
    path = PurePosixPath(relative)
    if (not relative or '\\' in relative or ':' in relative or path.is_absolute()
            or '..' in path.parts or str(path) != relative):
        raise ValueError('Derivation input requires a canonical repository path')
    raw_manifest = (HERE / 'frozen_derivation_inputs.json').read_bytes()
    if hashlib.sha256(raw_manifest).hexdigest() != 'e829d2512604fc44f5c4549b06ae05f4f7dbfacade6794201afe7181ed7b1cf0':
        raise ValueError('Frozen provenance manifest changed')
    manifest = json.loads(raw_manifest)
    if manifest.get('schema') != 1 or manifest.get('source_commit') != COHORT:
        raise ValueError('Derivation evidence lost its fixed source identity')
    entry = manifest['entries'].get(relative)
    if entry is None:
        return root / relative
    if (not (relative.startswith(('spec/', 'stdlib/', 'src/'))
             or (relative.startswith('tests/unit/xir/legacy_consumers/') and relative.endswith('.py'))
             or relative == 'tests/unit/xir/product_consumers/legacy_responsibilities.json')
            or not re.fullmatch('[0-9a-f]{64}', expected_lf_sha256)
            or entry['lf_sha256'] != expected_lf_sha256
            or entry['scope'] != 'DERIVATION_EVIDENCE_ONLY_NOT_CURRENT_PRODUCT_INPUT_OR_COMPLIANCE'):
        raise ValueError('Derivation evidence cannot change an independent input expectation')
    snapshot = HERE / entry['snapshot']
    if (snapshot.is_symlink() or (hasattr(snapshot, 'is_junction') and snapshot.is_junction())
            or not snapshot.resolve(strict=True).is_relative_to(HERE / 'frozen_derivation_inputs')):
        raise ValueError('Derivation evidence escaped its owned data directory')
    raw = snapshot.read_bytes()
    if (hashlib.sha256(raw).hexdigest() != entry['raw_sha256']
            or hashlib.sha256(raw.replace(b'\r\n', b'\n')).hexdigest() != expected_lf_sha256):
        raise ValueError('Frozen derivation evidence changed')
    return snapshot
