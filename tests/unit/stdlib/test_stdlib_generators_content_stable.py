import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stdlibgen"))


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


STDLIBGEN = load_module("xray_stdlibgen", ROOT / "tools" / "stdlibgen" / "stdlibgen.py")
ANALYZERGEN = load_module("xray_gen_stdlib_types", ROOT / "scripts" / "gen_stdlib_types.py")
COMPILER = None
TABLE_NAMES = (
    "DEF", "CONST_DEF", "HANDLE_DEF", "OBJECT_SHAPE_DEF", "ENUM_DEF", "TYPE_METHOD_DEF",
    "NATIVE_CLASS_DEF", "CLASS_DEF", "CLASS_METHOD_DEF", "CLASS_FIELD_DEF",
)


class ContentStableGeneratorTests(unittest.TestCase):
    def assert_content_stable(self, writer) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "generated.inc"
            self.assertTrue(writer(output, "first\n"))
            fixed_time = 1_700_000_000_000_000_000
            os.utime(output, ns=(fixed_time, fixed_time))

            self.assertFalse(writer(output, "first\n"))
            self.assertEqual(output.stat().st_mtime_ns, fixed_time)

            self.assertTrue(writer(output, "second\n"))
            self.assertEqual(output.read_text(encoding="utf-8"), "second\n")
            self.assertNotEqual(output.stat().st_mtime_ns, fixed_time)

    def test_stdlib_metadata_writer_preserves_unchanged_mtime(self) -> None:
        self.assert_content_stable(STDLIBGEN.write_if_changed)

    def test_analyzer_metadata_writer_preserves_unchanged_mtime(self) -> None:
        self.assert_content_stable(ANALYZERGEN.write_if_changed)

    def compile_metadata(self, metadata, checks: str = "") -> None:
        compiler = COMPILER or os.environ.get("CC") or next(
            (path for name in ("cl", "clang", "cc") if (path := shutil.which(name))), None
        )
        self.assertIsNotNone(compiler, "metadata portability test requires a C compiler")
        with tempfile.TemporaryDirectory(prefix="xray-metadata-c11.") as directory:
            root = Path(directory)
            (root / "metadata.h").write_text(
                STDLIBGEN.emit_defs_header(*metadata), encoding="utf-8"
            )
            (root / "methods.inc.c").write_text(
                STDLIBGEN.emit_aot_methods(metadata[0], metadata[1], metadata[4], metadata[6]),
                encoding="utf-8"
            )
            assertions = "".join(
                f'_Static_assert(XR_STDLIB_{name}_ENTRY_COUNT == {len(rows)}, "{name}");\n'
                for name, rows in zip(TABLE_NAMES, metadata, strict=True)
            )
            declarations = """
typedef enum {
    CG_AOT_RET_VALUE, CG_AOT_RET_I64, CG_AOT_RET_ENUM_I64, CG_AOT_RET_STR_BORROWED
} CgAotRetKind;
#define CG_AOT_STDLIB_VARIADIC UINT16_MAX
typedef struct {
    const char *module, *method;
    uint16_t argc;
    const char *shim, *arg_spec, *source_provider_arg_spec;
    CgAotRetKind ret_kind;
    const char *extern_decl;
    uint32_t enum_layout_id;
    const char *enum_name;
    const char *const *variant_names;
    uint16_t variant_count;
} CgAotStdlibMethod;
#include "methods.inc.c"
"""
            method_count = sum(e.aot_direct and e.aot_kind == "method" for e in metadata[0])
            constant_count = sum(bool(c.aot_const_kind) for c in metadata[1])
            assertions += (
                f'_Static_assert(CG_AOT_STDLIB_GENERATED_METHOD_COUNT == {method_count}, "methods");\n'
                f'_Static_assert(CG_AOT_STDLIB_GENERATED_CONST_COUNT == {constant_count}, "constants");\n'
            )
            checks += """
    if (cg_aot_stdlib_generated_const_at(-1) != NULL ||
        cg_aot_stdlib_generated_const_at(CG_AOT_STDLIB_GENERATED_CONST_COUNT) != NULL ||
        cg_aot_stdlib_generated_const_for_member("absent", "absent") != NULL ||
        cg_aot_stdlib_generated_module_has_constants("absent") ||
        cg_aot_stdlib_generated_has_builtin_direct_call("absent", "absent"))
        return 3;
"""
            source = root / "probe.c"
            source.write_text(
                '#include "metadata.h"\n#include <string.h>\n' + declarations + assertions +
                'int main(void) {\n' + checks + '\nreturn 0;\n}\n', encoding="utf-8"
            )
            msvc = Path(compiler).stem.lower() in ("cl", "clang-cl")
            executable = root / ("probe.exe" if os.name == "nt" else "probe")
            flags = (
                ["/nologo", "/std:c11", "/O2", "/W4", "/WX", "/I" + str(ROOT / "src/stdlib"),
                 str(source), "/Fe" + str(executable)] if msvc else
                ["-std=c11", "-pedantic-errors", "-O2", "-I", str(ROOT / "src/stdlib"),
                 str(source), "-o", str(executable)]
            )
            result = subprocess.run(
                [compiler, *flags], cwd=root, capture_output=True, text=True,
                encoding="utf-8", errors="replace", env={**os.environ, "VSLANG": "1033"}, timeout=60
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_metadata_is_c11_with_zero_logical_counts(self) -> None:
        self.compile_metadata([[] for _ in TABLE_NAMES])

    def test_repository_metadata_compiles_as_c11(self) -> None:
        self.compile_metadata(STDLIBGEN.parse_def_metadata(ROOT))

    def test_empty_nested_tables_preserve_nonempty_parent_rows(self) -> None:
        metadata = [[] for _ in TABLE_NAMES]
        metadata[2] = [STDLIBGEN.StdlibHandleEntry("sample", "Handle", "", (), "public")]
        metadata[3] = [
            STDLIBGEN.StdlibObjectShapeEntry("sample", "Shape", "", (), True, "public")
        ]
        metadata[4] = [STDLIBGEN.StdlibEnumEntry("sample", "Enum", "", (), "public")]
        self.compile_metadata(metadata, """
    if (xr_stdlib_handle_def_entries[0].field_count != 0 ||
        xr_stdlib_object_shape_def_entries[0].field_count != 0 ||
        xr_stdlib_enum_def_entries[0].variant_count != 0)
        return 1;
    if (strcmp(xr_stdlib_handle_def_entries[0].name, "Handle") != 0 ||
        strcmp(xr_stdlib_object_shape_def_entries[0].name, "Shape") != 0 ||
        strcmp(xr_stdlib_enum_def_entries[0].name, "Enum") != 0)
        return 2;
"""
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--cc")
    options, remaining = parser.parse_known_args()
    COMPILER = options.cc
    unittest.main(argv=[sys.argv[0], *remaining])
