"""Reject drift and mixed product assets before accepting supplied CLI evidence."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from fixed_product_inputs import FixedProductInputs


class FixedProductInputControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.binary = self.root / 'xray.exe'
        self.path = self.root / 'inputs.json'
        files = {'xray.exe': b'guard fixture only', 'stdlib/prelude.xr': b'fixed declaration',
                 'xir-runtime-sdk/xir.h': b'fixed SDK declaration'}
        for name, raw in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        self.data = {'schema': 1, 'source_commit': 'a' * 40, 'binary': 'xray.exe',
                     'stdlib_root': 'stdlib',
                     'files': {name: hashlib.sha256(raw).hexdigest() for name, raw in files.items()},
                     'version': {'product': 'xray-lang', 'commit': 'a' * 40, 'dirty': False,
                                 'target': 'windows-x86_64', 'features': ['vm', 'aot']}}
        self.data['binary_sha256'] = self.data['files']['xray.exe']
        self.write()

    def write(self):
        self.path.write_text(json.dumps(self.data), encoding='utf-8')

    def load(self):
        return FixedProductInputs(self.path, self.binary)

    def test_exact_complete_pair_and_version(self):
        bound = self.load()
        bound.require_version(deepcopy(self.data['version']))
        bound.verify()
        self.assertEqual(3, bound.evidence()['file_count'])
        self.assertEqual(self.root / 'stdlib', bound.stdlib)

    def test_binary_drift(self):
        bound = self.load()
        self.binary.write_bytes(b'drift')
        with self.assertRaisesRegex(ValueError, 'changed or is unlisted'):
            bound.verify()

    def test_stdlib_and_sdk_drift(self):
        for name in ('stdlib/prelude.xr', 'xir-runtime-sdk/xir.h'):
            with self.subTest(name=name):
                path = self.root / name
                raw = path.read_bytes()
                path.write_bytes(b'drift')
                with self.assertRaises(ValueError):
                    self.load()
                path.write_bytes(raw)

    def test_escape_absolute_and_noncanonical_paths(self):
        original = deepcopy(self.data)
        for name in ('../outside', '/absolute', 'C:/absolute', 'stdlib/../x', './x',
                     'stdlib//x', 'stdlib\\x', 'stdlib/x.', 'stdlib/x '):
            with self.subTest(name=name):
                self.data = deepcopy(original)
                self.data['files'][name] = '0' * 64
                self.write()
                with self.assertRaisesRegex(ValueError, 'canonical and relative'):
                    self.load()

    def test_wrong_argument_binary(self):
        other = self.root / 'other.exe'
        other.write_bytes(self.binary.read_bytes())
        with self.assertRaisesRegex(ValueError, 'not the bound public binary'):
            FixedProductInputs(self.path, other)

    def test_wrong_actual_version(self):
        bound = self.load()
        for key, value in (('dirty', True), ('commit', 'b' * 40), ('target', 'other'),
                           ('features', ['vm'])):
            version = deepcopy(self.data['version'])
            version[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'actual CLI version'):
                bound.require_version(version)

    def test_manifest_cannot_declare_a_dirty_or_different_commit(self):
        for key, value in (('dirty', True), ('commit', 'b' * 40)):
            self.data['version'][key] = value
            self.write()
            with self.assertRaisesRegex(ValueError, 'clean public CLI version'):
                self.load()
            self.data['version'].update(dirty=False, commit='a' * 40)

    def test_unlisted_asset_or_missing_file(self):
        bound = self.load()
        extra = self.root / 'runtime.dll'
        extra.write_bytes(b'outside table')
        with self.assertRaises(ValueError):
            bound.verify()
        extra.unlink()
        (self.root / 'xir-runtime-sdk/xir.h').unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            bound.verify()

    def test_midrun_manifest_refresh(self):
        bound = self.load()
        self.path.write_bytes(self.path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'manifest changed'):
            bound.verify()

    def test_duplicate_case_alias_and_json_member(self):
        self.data['files']['XRAY.EXE'] = self.data['files']['xray.exe']
        self.write()
        with self.assertRaisesRegex(ValueError, 'aliases'):
            self.load()
        self.path.write_text('{"schema":1,"schema":1}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'duplicate product manifest member'):
            self.load()

    def test_link_is_rejected_without_following_target(self):
        from unittest.mock import patch
        import fixed_product_inputs
        real = fixed_product_inputs._linked
        with patch.object(fixed_product_inputs, '_linked',
                          side_effect=lambda path: path.name == 'prelude.xr' or real(path)):
            with self.assertRaisesRegex(ValueError, 'must not contain links'):
                self.load()


if __name__ == '__main__':
    unittest.main()
