"""Retired object identities do not reserve ordinary nominal declaration names."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import check_structural_object_json_map_boundary as guard
import check_parallel_surface_convergence as parallel_guard


class ObjectBoundaryGuard(unittest.TestCase):
    notice = '不得保旧 Json／JsonValue 别名、旧 raw fallback、双签名或迁移开关。'

    def write_notice(self, root: Path, text: str) -> None:
        path = root / guard.RETIRED_JSON_NOTICE_PATH
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def test_nominal_record_name_and_real_retired_identities(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "spec").mkdir()
            (root / "src").mkdir()
            (root / "spec/example.xr").write_text(
                'struct Record { value: i64 }\nconst value=Record{value:42}\n', encoding='utf-8')
            self.assertFalse(any(guard.collect(root).values()))
            (root / "spec/removed.xr").write_text('const old=Type.record\n', encoding='utf-8')
            (root / "src/removed.h").write_text(
                'enum Old { AST_OBJECT_LITERAL, XR_BK_STRUCT_OBJECT, AST_SET_LITERAL };\n', encoding='utf-8')
            findings = guard.collect(root)
            self.assertEqual(len(findings['PUBLIC_RECORD_SURFACE']), 1)
            self.assertEqual(len(findings['DELETED_STRUCTURAL_OBJECT_IDENTITY']), 1)

    def test_canonical_retirement_notice_is_not_a_public_type(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_notice(root, '- 所有公共操作必须认证真实声明。' + self.notice + '\n')
            self.assertFalse(any(guard.collect(root).values()))

    def test_active_legacy_uses_survive_the_notice_on_the_same_line(self):
        cases = (
            (' `const old: Json = null`', 'PUBLIC_LEGACY_JSON_TYPE'),
            (' `const old: JsonValue = null`', 'PUBLIC_LEGACY_JSON_TYPE'),
            (' `Json.parse(text)`', 'PUBLIC_LEGACY_JSON_NAMESPACE'),
            (' `@derive(Json)`', 'PUBLIC_LEGACY_JSON_TYPE'),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for code, rule in cases:
                with self.subTest(code=code):
                    self.write_notice(root, self.notice + code + '\n')
                    findings = guard.collect(root)
                    self.assertEqual(len(findings[rule]), 1)
                    self.assertEqual(findings[rule][0]['line'], 1)

    def test_active_fenced_code_in_the_canonical_document_is_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_notice(root, self.notice + '\n```xr\nconst old: Json = null\n'
                              'const alias: JsonValue = null\nJson.parse(text)\n@derive(Json)\n```\n')
            findings = guard.collect(root)
            self.assertEqual([hit['line'] for hit in findings['PUBLIC_LEGACY_JSON_TYPE']], [3, 4, 5, 6])
            self.assertEqual([hit['line'] for hit in findings['PUBLIC_LEGACY_JSON_NAMESPACE']], [5])

    def test_other_prohibitions_and_other_documents_are_not_exempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_notice(root, '不得继续使用 Json 或 JsonValue。\n')
            other = root / 'spec/another.md'
            other.write_text(self.notice + '\n', encoding='utf-8')
            findings = guard.collect(root)
            self.assertEqual(len(findings['PUBLIC_LEGACY_JSON_TYPE']), 2)


class ParallelSurfaceGuard(unittest.TestCase):
    notice = '首发布指同步点上的线性化顺序，不是墙钟、worker ID 或源码顺序。'
    canonical = Path('spec/source/accepted/331-language-surface-convergence-cn.md')

    def write_notice(self, root: Path, text: str) -> None:
        path = root / self.canonical
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def findings(self, root: Path) -> list[str]:
        return (parallel_guard.check_public_parallel_recommendations(root)
                + parallel_guard.check_legacy_public_text(root))

    def test_canonical_first_publication_definition_is_not_a_recommendation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_notice(root, '- ' + self.notice + '不能在首个 Error 到达时提前结束 join。\n')
            self.assertEqual(self.findings(root), [])

    def test_same_line_recommendations_and_legacy_syntax_still_fail(self):
        cases = (
            ('建议按 worker_id 排序。', 'worker-id parallel recommendation'),
            ('建议按 worker ID 排序。', 'worker-id parallel recommendation'),
            ('使用 lane arrays。', 'manual lane-array recommendation'),
            ('`lanes[worker]`', 'manual lane indexing recommendation'),
            ('`parallel for value in values {}`', 'legacy parallel grammar'),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for code, label in cases:
                with self.subTest(code=code):
                    self.write_notice(root, self.notice + code + '\n')
                    findings = self.findings(root)
                    self.assertEqual(len(findings), 1)
                    self.assertIn(label, findings[0])

    def test_fenced_recommendations_remain_active(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_notice(root, self.notice + '\n```xr\nconst worker_id = 0\n'
                              'const slot = lanes[0]\nparallel for x in xs {}\n```\n')
            findings = self.findings(root)
            self.assertEqual(len(findings), 3)
            self.assertTrue(any(':3:' in hit and 'worker-id' in hit for hit in findings))
            self.assertTrue(any(':4:' in hit and 'lane indexing' in hit for hit in findings))
            self.assertTrue(any(':5:' in hit and 'legacy parallel grammar' in hit for hit in findings))

    def test_other_negations_and_documents_have_no_exemption(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.write_notice(root, '不是建议按 worker ID 排序。\n')
            (root / 'spec/another.md').write_text(self.notice + '\n', encoding='utf-8')
            self.assertEqual(len(self.findings(root)), 2)


if __name__ == '__main__':
    unittest.main()
