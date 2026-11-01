"""Strict, independent seven-file crypto VM/native catalog acceptance."""
from __future__ import annotations
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from typing import Callable

SOURCE = "demos/07-advanced/crypto_entropy.xr"
# Derived from the independently sealed original inputs, never a runtime report.
ORIGINAL = (('tests/regression/10_stdlib/1400_crypto_hash.xr',
  ('test_md5_empty_string',
   'test_md5_hello',
   'test_md5_hello_world',
   'test_md5_numbers',
   'test_md5_long_string',
   'test_sha1_empty_string',
   'test_sha1_hello',
   'test_sha1_hello_world',
   'test_sha1_long_string',
   'test_sha256_empty_string',
   'test_sha256_hello',
   'test_sha256_hello_world',
   'test_sha256_long_string',
   'test_hash_lengths')),
 ('tests/regression/10_stdlib/1401_crypto_hmac.xr',
  ('test_hmac_sha256_basic',
   'test_hmac_sha256_empty_message',
   'test_hmac_sha256_complex_key',
   'test_hmac_sha256_rfc4231_jefe_vector',
   'test_hmac_sha256_long_data',
   'test_hmac_sha1_basic',
   'test_hmac_sha1_empty_message',
   'test_hmac_sha1_length',
   'test_hmac_md5_basic',
   'test_hmac_md5_empty_message',
   'test_hmac_md5_length',
   'test_hmac_unsupported_algorithm_returns_null')),
 ('tests/regression/10_stdlib/1402_crypto_random.xr',
  ('test_random_bytes_length_8',
   'test_random_bytes_length_16',
   'test_random_bytes_length_32',
   'test_random_bytes_uniqueness',
   'test_random_bytes_hex_format',
   'test_uuid_length',
   'test_uuid_format',
   'test_uuid_parts_length',
   'test_uuid_uniqueness',
   'test_uuid_multiple_uniqueness')),
 ('tests/regression/10_stdlib/1403_crypto_sha512.xr',
  ('test_sha512_empty_string',
   'test_sha512_hello',
   'test_sha512_hello_world',
   'test_sha512_length',
   'test_sha512_long_string',
   'test_hmac_sha512_basic',
   'test_hmac_sha512_empty_message',
   'test_hmac_sha512_known_vector')),
 ('tests/regression/10_stdlib/1404_crypto_aes.xr',
  ('test_encrypt_decrypt_basic',
   'test_encrypt_decrypt_empty',
   'test_encrypt_decrypt_long_text',
   'test_encrypt_output_is_hex',
   'test_encrypt_different_each_time',
   'test_decrypt_wrong_key',
   'test_encrypt_decrypt_exactly_16_bytes',
   'test_encrypt_decrypt_unicode')),
 ('tests/regression/10_stdlib/1405_crypto_utils.xr',
  ('test_timing_safe_equal_same',
   'test_timing_safe_equal_different',
   'test_timing_safe_equal_different_length',
   'test_timing_safe_equal_empty',
   'test_timing_safe_equal_hmac_verify',
   'test_timing_safe_equal_hmac_mismatch',
   'test_all_hash_lengths',
   'test_all_hmac_lengths')),
 ('tests/regression/10_stdlib/1406_crypto_aead.xr',
  ('test_reference_wire_forms_decrypt_to_their_plaintext',
   'test_every_padding_length_round_trips',
   'test_a_different_key_does_not_authenticate',
   'test_two_encryptions_of_one_plaintext_differ',
   'test_tampering_anywhere_is_refused',
   'test_a_wire_form_of_the_wrong_length_is_a_typed_error',
   'test_a_non_hex_wire_form_answers_null')))
REPORT_KEYS = {"schema", "suites", "discovered", "executed", "passed", "skipped", "failed"}
ENTRY_KEYS = {"name", "function", "role", "timeout", "executed", "passed"}

def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def valid_digest(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)

def validate_catalog(value: dict, *, expected: bool = False) -> dict:
    keys = {"schema", "suites"} if expected else REPORT_KEYS
    if not isinstance(value, dict) or set(value) != keys or type(value["schema"]) is not int or value["schema"] != 1:
        raise ValueError("crypto suite requires its exact catalog schema")
    suites = value["suites"]
    if not isinstance(suites, list) or len(suites) != 7:
        raise ValueError("crypto suite requires seven complete source inputs")
    for suite, (path, names) in zip(suites, ORIGINAL):
        hash_key = "sourceCanonicalSha256" if expected else "sourceSha256"
        if not isinstance(suite, dict) or set(suite) != {"source", hash_key, "entries"} or suite["source"] != path or not valid_digest(suite[hash_key]):
            raise ValueError("crypto suite source identity differs")
        entries = suite["entries"]
        if not isinstance(entries, list) or len(entries) != len(names):
            raise ValueError("crypto suite is missing original test entries")
        functions = set()
        for entry, name in zip(entries, names):
            keys = {"name", "role", "timeout"} if expected else ENTRY_KEYS
            if not isinstance(entry, dict) or set(entry) != keys or entry["name"] != name or type(entry["role"]) is not int or entry["role"] != 1 or type(entry["timeout"]) is not int or entry["timeout"] != 0:
                raise ValueError("crypto suite names, roles or timeouts differ")
            if not expected:
                function = entry["function"]
                if type(function) is not int or not 0 <= function <= 0xffffffff or function in functions:
                    raise ValueError("crypto suite needs distinct actual uint32 function IDs per Program")
                functions.add(function)
                if entry["executed"] is not True or entry["passed"] is not True:
                    raise ValueError("crypto suite did not execute every test successfully")
    if not expected and (any(type(value[key]) is not int for key in REPORT_KEYS - {"schema", "suites"}) or
                         tuple(value[key] for key in ("discovered", "executed", "passed", "skipped", "failed")) != (67, 67, 67, 0, 0)):
        raise ValueError("crypto suite requires 67 discovered, executed and passing tests without skips")
    return value

def expected_catalog(case: dict, root: Path) -> dict | None:
    selected = case.get("crypto_suite")
    if selected is None:
        if case.get("path") == SOURCE and case.get("expect") == "pass":
            raise ValueError("crypto demo also requires its complete seven-file test suite")
        return None
    if case.get("path") != SOURCE or case.get("mode") not in ("run", "build-run") or selected != {"schema": 1, "catalog": "tests/smoke/expected/crypto-catalog.json"} or type(selected.get("schema")) is not int:
        raise ValueError("crypto suite mode is restricted to the original crypto demo")
    expected = validate_catalog(json.loads((root / selected["catalog"]).read_text(encoding="utf-8")), expected=True)
    for suite in expected["suites"]:
        raw = (root / suite["source"]).read_bytes()
        if hashlib.sha256(raw.replace(b"\r\n", b"\n")).hexdigest() != suite.pop("sourceCanonicalSha256"):
            raise ValueError("crypto suite source differs from the independent expected input")
        suite["sourceSha256"] = hashlib.sha256(raw).hexdigest()
    return expected

@dataclass
class CryptoCommands:
    vm: Path | None = None
    native: Path | None = None
    generated_dir: Path | None = None
    verified_vm: tuple[dict, dict] | None = None

    def execute(self, case: dict, root: Path, logs: Path, invoke: Callable) -> dict:
        expected = expected_catalog(case, root)
        if expected is None:
            raise ValueError("crypto suite is not configured")
        engine = "native" if case["mode"] == "build-run" else "vm"
        driver = self.vm if engine == "vm" else self.native
        if driver is None or not driver.is_file():
            raise ValueError("crypto suite requires its real executable")
        report = logs / ("crypto-" + engine + ".json")
        generated = logs / "crypto-generated"
        if report.exists() or generated.exists():
            raise ValueError("crypto suite outputs must belong to this invocation")
        names = [f"crypto_suite_{i}.c" for i in range(7)]
        if engine == "vm":
            generated.mkdir()
            arguments = [driver, root, generated, report]
        else:
            if self.verified_vm is None or self.generated_dir is None:
                raise ValueError("crypto native suite requires the verified VM products")
            if any(not (self.generated_dir / name).is_file() or digest(self.generated_dir / name) != self.verified_vm[1][name] for name in names):
                raise ValueError("crypto native suite C differs from the actual VM SourceProducts")
            arguments = [driver, report]
        result = invoke("crypto-suite-" + engine, arguments)
        if result.timed_out or not result.ok or result.stderr or not report.is_file():
            raise ValueError("crypto suite did not publish its actual successful execution")
        actual = validate_catalog(json.loads(report.read_text(encoding="utf-8")))
        logical = {"schema": 1, "suites": [dict(suite, entries=[{key: row[key] for key in ("name", "role", "timeout")} for row in suite["entries"]]) for suite in actual["suites"]]}
        if logical != expected:
            raise ValueError("crypto suite source or catalog differs from its independent inputs")
        prefix = "VM" if engine == "vm" else "native"
        lines = [f"{prefix} {name} PASS" for _, entries in ORIGINAL for name in entries]
        lines.append("crypto catalog: 67 PASS, 0 SKIP, 0 FAIL")
        if result.stdout.decode("utf-8").splitlines() != lines:
            raise ValueError("crypto suite did not report each original test execution")
        if engine == "vm":
            if sorted(path.name for path in generated.glob("*.c")) != names or any(not (generated / name).stat().st_size for name in names):
                raise ValueError("crypto VM suite did not emit all seven real C projections")
            hashes = {name: digest(generated / name) for name in names}
            self.verified_vm = (actual, hashes)
        else:
            if actual != self.verified_vm[0]:
                raise ValueError("crypto VM/native actual catalog IDs or outcomes differ")
            hashes = self.verified_vm[1]
        return {"status": "pass", "engine": engine, "executed": True, "catalog": actual,
                "report": str(report), "report_sha256": digest(report), "generated_c_sha256": hashes,
                "matches_vm": engine == "native"}

def passed(case: dict, row: dict) -> bool:
    if case.get("path") != SOURCE:
        return case.get("crypto_suite") is None
    suite = row.get("crypto_suite", {})
    engine = "native" if case.get("mode") == "build-run" else "vm"
    commands = [item for item in row.get("commands", []) if item.get("stage") == "crypto-suite-" + engine]
    if not case.get("crypto_suite") or row.get("executed") is not True or len(commands) != 1 or commands[0].get("exit_code") != 0 or commands[0].get("timed_out") is not False:
        return False
    try:
        validate_catalog(suite.get("catalog"))
    except (ValueError, KeyError, TypeError):
        return False
    hashes = suite.get("generated_c_sha256", {})
    return (suite.get("status") == "pass" and suite.get("executed") is True and suite.get("engine") == engine and
            (engine != "native" or suite.get("matches_vm") is True) and
            isinstance(hashes, dict) and set(hashes) == {f"crypto_suite_{i}.c" for i in range(7)} and all(valid_digest(value) for value in hashes.values()))
