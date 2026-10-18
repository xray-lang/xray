"""Read original assertion data without reviving its retired execution path."""
from pathlib import Path
import hashlib
import importlib.util
import json
import sys

HERE = Path(__file__).resolve().parent
DATA_ROOT = HERE.parents[3] / 'testdata/legacy_consumers/original_assertions'
COHORT = '8bbf31427772e0a9245a5126144ce421cd6b8e86'


def ledger_path() -> Path:
    return DATA_ROOT / 'tests/unit/xir/product_consumers/legacy_responsibilities.json'


def load(name: str):
    raw_manifest = (HERE / 'original_assertions.json').read_bytes()
    if hashlib.sha256(raw_manifest).hexdigest() != 'c37cf40c77fe43191e056873da2e80007509ff1916ab84d46dbde49c8d50a65c':
        raise ValueError('Frozen provenance manifest changed')
    manifest = json.loads(raw_manifest)
    if (manifest.get('schema') != 1 or manifest.get('source_commit') != COHORT
            or manifest.get('scope') != 'FROZEN_ORIGINAL_CENSUS_DERIVATION_DATA_NOT_PRODUCT_IMPLEMENTATION'):
        raise ValueError('Original census lost its fixed derivation identity')
    root = DATA_ROOT
    for relative, item in manifest['files'].items():
        path = root / relative
        if (path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())
                or not path.resolve(strict=True).is_relative_to(root)):
            raise ValueError('Original assertion evidence escaped its data directory')
        if hashlib.sha256(path.read_bytes()).hexdigest() != item['snapshot_sha256']:
            raise ValueError('Original assertion data changed: ' + relative)
    spec = importlib.util.spec_from_file_location(name, root / 'scripts/source_product_consumer_inventory.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module
