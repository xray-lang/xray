"""Shared sources must reject drift rather than invent language support."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import gen_language_docs as docs
import spec_sources as source
import check_spec_xray_snippets as snippets


class SharedSourceTests(unittest.TestCase):
    def test_support_report_does_not_promote_unrun_or_baselined_failures(self):
        report = {"conforms": False, "gate_pass": True, "results": [
            {"key": "checked", "mode": "check", "status": "CHECK_PASS"},
            {"key": "known", "mode": "run", "status": "RUN_FAIL", "baselined": True,
             "vm_pass": False, "native_build_pass": False, "native_pass": False,
             "native_execution": "NOT_RUN"}]}
        rendered = snippets.support_table(report)
        self.assertIn("checked | check | CHECK_PASS | NOT_RUN | NOT_RUN | NOT_RUN", rendered)
        self.assertIn("known | run | RUN_FAIL | FAIL | FAIL | NOT_RUN | yes", rendered)
        self.assertIn("conform: **False**", rendered)

    def test_shared_example_identity_survives_prose_movement(self):
        text = "<!-- xr-source:example stable-name -->\n```xray @check\nprint(1)\n```\n<!-- /xr-source -->\n"
        for prefix in ("", "Introductory prose\n\n"):
            blocks, errors = snippets.parse_document(prefix + text)
            self.assertFalse(errors)
            self.assertEqual(blocks[0]["source_key"], "stable-name")
            self.assertEqual(blocks[0]["code"], "print(1)")

    def test_translation_cannot_change_literals_or_swallow_tokens(self):
        entry = {"code": 'print("// literal") // comment\nprint(2)',
                 "trivia": {"en": {"0": "// translated\n"}}}
        self.assertTrue(source.localized_code(entry, "en").startswith("// translated\n"))
        for value in ('other()', '// swallowed', '/* unclosed'):
            entry["trivia"]["en"]["0"] = value
            with self.assertRaises(ValueError):
                source.localized_code(entry, "en")

    def test_translation_preserves_token_boundaries(self):
        entry = {"code": 'var name = 1', "trivia": {"en": {"1": ""}}}
        with self.assertRaises(ValueError):
            source.localized_code(entry, "en")
        with self.assertRaisesRegex(ValueError, "line ending"):
            source.localized_code({"code": "return\n1", "trivia": {"en": {"1": " "}}}, "en")

    def test_grammar_has_one_definition_per_name(self):
        with self.assertRaisesRegex(ValueError, "duplicate grammar"):
            source.grammar_rules("Item ::= 'a'\nItem ::= 'b'\n")

    def test_duplicate_json_keys_reject(self):
        with self.assertRaisesRegex(ValueError, "duplicate source key"):
            json.loads('{"rule":1,"rule":2}', object_pairs_hook=source.unique_object)

    def test_all_shared_examples_keep_bilingual_tokens_and_expectations(self):
        sources = source.Sources(ROOT)
        for key, entry in sources.examples.items():
            with self.subTest(key=key):
                cn = source.lexical_parts(source.localized_code(entry, "cn"))[0]
                en = source.lexical_parts(source.localized_code(entry, "en"))[0]
                self.assertEqual(cn, en)
                self.assertEqual(sources.render("example", key, "cn").count("```output"),
                                 sources.render("example", key, "en").count("```output"))

    def test_current_projections_are_exact(self):
        for path, expanded in source.render_sections(ROOT).items():
            self.assertEqual(path.read_text(encoding="utf-8"), expanded, str(path))

    def test_missing_or_handwritten_sources_reject(self):
        sources = source.Sources(ROOT)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "section.md"
            for body in (
                "```xray\nprint(1)\n```",
                "```ebnf\nItem ::= 'a'\n```",
                "<!-- xr-source:example absent -->\n<!-- /xr-source -->",
                "<!-- xr-source:example absent -->\nunterminated",
            ):
                path.write_text("<!-- xr-spec:cn -->\n" + body + "\n<!-- /xr-spec:cn -->\n", encoding="utf-8")
                with self.subTest(body=body), self.assertRaises(ValueError):
                    sources.expand(path)

    def test_check_reports_stale_projection_without_rewriting(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            shutil.copytree(ROOT / "spec/source", root / "spec/source")
            for relative in ("src/frontend/lexer/xkeywords.def", "src/shared/xr_exact_scalar_registry.def"):
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, target)
            path = root / "spec/source/sections/021-appendix-b-keyword-index.md"
            before = path.read_text(encoding="utf-8").replace("| `await` |", "| `invented` |")
            path.write_text(before, encoding="utf-8")
            errors = docs.generate(root, True, "strict")
            self.assertTrue(any("stale generated output" in error for error in errors))
            self.assertEqual(path.read_text(encoding="utf-8"), before)

    def test_projection_must_stay_inside_language_block(self):
        sources = source.Sources(ROOT)
        key = next(iter(sources.examples))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "section.md"
            for body in (
                f"<!-- xr-spec:cn -->\n<!-- /xr-spec:cn -->\n<!-- xr-source:example {key} -->\n<!-- /xr-source -->",
                f"<!-- xr-spec:cn -->\n<!-- xr-source:example {key} -->\n<!-- /xr-spec:cn -->\n<!-- /xr-source -->",
            ):
                path.write_text(body, encoding="utf-8")
                with self.subTest(body=body), self.assertRaises(ValueError):
                    sources.expand(path)


if __name__ == "__main__":
    unittest.main()
