"""Keep original expectation provenance independent of current product files."""
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import frozen_derivation_inputs as derivation
import frozen_original_census as census


class FrozenProvenanceControls(unittest.TestCase):
    def test_complete_original_census_is_recomputed(self):
        module = census.load('fixed_original_census_control')
        actual, _ = module.inventory()
        self.assertEqual(actual, json.loads(census.ledger_path().read_bytes()))
        self.assertEqual(len(actual['source']), 108)
        self.assertEqual(len(actual['other_consumer_files']), 26)
        self.assertEqual(sum(len(v['functions']) for v in actual['other_consumer_files'].values()), 160)

    def test_reference_sources_are_outside_active_tests(self):
        self.assertFalse(census.DATA_ROOT.is_relative_to(census.HERE.parents[3] / 'tests'))

    def test_frozen_derivations_have_the_original_input_hashes(self):
        data = json.loads((derivation.HERE / 'frozen_derivation_inputs.json').read_bytes())
        self.assertGreaterEqual(len(data['entries']), 7)
        for relative, item in data['entries'].items():
            with self.subTest(relative=relative):
                path = derivation.resolve(derivation.HERE.parents[3], relative, item['lf_sha256'])
                self.assertTrue(path.name.endswith('.input'))
                self.assertEqual(hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest(), item['lf_sha256'])

    def test_unlisted_inputs_use_actual_product_tree(self):
        root = derivation.HERE.parents[3]
        self.assertEqual(derivation.resolve(root, 'stdlib/prelude.xr', '0' * 64), root / 'stdlib/prelude.xr')

    def test_noncanonical_paths_are_rejected(self):
        for relative in ('../x', '/x', 'C:/x', 'spec/../x', 'spec\\x', './spec/x'):
            with self.subTest(relative=relative), self.assertRaises(ValueError):
                derivation.resolve(derivation.HERE.parents[3], relative, '0' * 64)

    def test_derivation_cannot_change_the_expected_digest(self):
        data = json.loads((derivation.HERE / 'frozen_derivation_inputs.json').read_bytes())
        with self.assertRaisesRegex(ValueError, 'independent input expectation'):
            derivation.resolve(derivation.HERE.parents[3], next(iter(data['entries'])), '0' * 64)

    def test_derivation_snapshot_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copy(derivation.HERE / 'frozen_derivation_inputs.json', root)
            shutil.copytree(derivation.HERE / 'frozen_derivation_inputs', root / 'frozen_derivation_inputs')
            data = json.loads((root / 'frozen_derivation_inputs.json').read_bytes())
            relative, item = next(iter(data['entries'].items()))
            (root / item['snapshot']).write_bytes(b'changed evidence')
            with patch.object(derivation, 'HERE', root), self.assertRaisesRegex(ValueError, 'evidence changed'):
                derivation.resolve(root, relative, item['lf_sha256'])

    def test_manifest_cannot_drop_census_or_derivation_entries(self):
        for module, name, operation in (
                (census, 'original_assertions.json', lambda: census.load('invalid_census')),
                (derivation, 'frozen_derivation_inputs.json', lambda: derivation.resolve(Path('.'), 'spec/source/x.md', '0' * 64))):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / name).write_text('{}', encoding='utf-8')
                with patch.object(module, 'HERE', root), self.assertRaisesRegex(ValueError, 'manifest changed'):
                    operation()

    def test_census_snapshot_tampering_and_missing_files_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'data'
            shutil.copytree(census.DATA_ROOT, root)
            path = root / 'source_baseline_manifest.bin'
            path.write_bytes(b'changed original manifest')
            with patch.object(census, 'DATA_ROOT', root), self.assertRaisesRegex(ValueError, 'assertion data changed'):
                census.load('changed_census')
            path.unlink()
            with patch.object(census, 'DATA_ROOT', root), self.assertRaises(FileNotFoundError):
                census.load('missing_census')


if __name__ == '__main__':
    unittest.main()
