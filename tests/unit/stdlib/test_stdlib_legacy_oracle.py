import importlib.util
import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "stdlib_legacy_oracle", ROOT / "scripts/stdlib_legacy_oracle.py"
)
assert SPEC and SPEC.loader
ORACLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ORACLE)


class LegacyOracleObservationTests(unittest.TestCase):
    def test_canonical_observation_rejects_lossy_or_incomplete_rows(self):
        with self.assertRaisesRegex(ValueError, "missing fields"):
            ORACLE.canonical_observation({"case": "x", "outcome": "value"})
        with self.assertRaisesRegex(ValueError, "effects must be an object"):
            ORACLE.canonical_observation(
                {"case": "x", "outcome": "value", "value": 1, "error": None, "effects": []}
            )

    def test_parse_observations_is_jsonl_and_case_unique(self):
        row = {"case": "roundtrip", "outcome": "value", "value": True, "error": None, "effects": {}}
        parsed = ORACLE.parse_observations((json.dumps(row) + "\n").encode(), "probe")
        self.assertEqual(parsed, [row])
        with self.assertRaisesRegex(ValueError, "duplicate observation"):
            ORACLE.parse_observations(
                (json.dumps(row) + "\n" + json.dumps(row) + "\n").encode(), "probe"
            )


if __name__ == "__main__":
    unittest.main()
