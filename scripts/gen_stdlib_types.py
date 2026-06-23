#!/usr/bin/env python3
"""
xray - Lightweight typed scripting with native concurrency
https://www.xray-lang.org

Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
Licensed under the MIT License

gen_stdlib_types.py - Generate analyzer builtin type declarations

KEY CONCEPT:
  Uses stdlib/defs/*.def as the primary declaration source for migrated module
  functions, constants, and handles, then falls back to C annotations for
  builtin types and module members not yet migrated to declarative metadata.

  Supports two output modes:
    --embed (default): Generate xanalyzer_builtins_generated.h (for stdlib)
    --xrd <file.c>:    Generate .xrd declaration file (for third-party modules)

USAGE:
  python3 scripts/gen_stdlib_types.py                    # embed mode
  python3 scripts/gen_stdlib_types.py --check            # verify generated header
  python3 scripts/gen_stdlib_types.py --xrd my_db.c      # xrd mode

This ensures type declarations stay in sync with runtime implementations.
"""

import argparse
import difflib
import os
import re
import sys
from pathlib import Path

# Project root
PROJECT_ROOT = Path(__file__).parent.parent

# Source directories to scan (embed mode)
STDLIB_DIRS = [
    PROJECT_ROOT / "stdlib",
    PROJECT_ROOT / "src" / "module",
]

# Output files (embed mode)
OUTPUT_FILE = PROJECT_ROOT / "src" / "frontend" / "analyzer" / "xanalyzer_builtins_generated.h"
LSP_OUTPUT_FILE = PROJECT_ROOT / "src" / "app" / "lsp" / "xlsp_stdlib_generated.inc"
STDLIBGEN_TOOL_DIR = PROJECT_ROOT / "tools" / "stdlibgen"

# Pattern to match method/function definitions in C
# e.g., XR_DEFINE_BUILTIN(array_push, "push", "(item: T): int", "Push item to end")
# The doc string may use C adjacent-string concatenation across lines;
# _join_adjacent_strings() collapses those before this regex runs.
METHOD_PATTERN = re.compile(
    r'XR_DEFINE_BUILTIN\s*\(\s*'
    r'(\w+)\s*,\s*'           # C function name
    r'"([^"]+)"\s*,\s*'       # Method/function name
    r'"([^"]+)"\s*,\s*'       # Signature
    r'"([^"]+)"\s*\)'         # Documentation
)

# Collapse C adjacent string literals ("a" "b" -> "ab") so that
# multi-line XR_DEFINE_BUILTIN doc strings match METHOD_PATTERN.
_ADJACENT_STR = re.compile(r'"\s*\n?\s*"')


def _join_adjacent_strings(text: str) -> str:
    return _ADJACENT_STR.sub('', text)

# Pattern to match module-level constants registered at runtime, e.g.
#   xr_module_add_export(isolate, mod, "DEBUG", xr_int(XR_LOG_DEBUG))
#   xr_module_add_export(isolate, mod, "PI",    xr_float(M_PI))
#   xr_module_add_export(isolate, mod, "sep",   xrs_string_value_c(isolate, "/"))
# The constructor (xr_int / xr_float / xr_bool / xrs_string_value_c) is the
# single signal we have for the constant's runtime type — capture it so we
# can synthesise a typed entry in the analyzer table without forcing every
# stdlib module to spell out the type via XR_DEFINE_BUILTIN.
EXPORT_PATTERN = re.compile(
    r'xr_module_add_export\s*\(\s*'
    r'\w+\s*,\s*'                                         # isolate arg
    r'\w+\s*,\s*'                                         # module arg
    r'"([^"]+)"\s*,\s*'                                   # export name
    r'(xr_int|xr_float|xr_bool|xrs_string_value_c)\s*\('  # value constructor
)

# Map runtime constructors to xray surface type names.
EXPORT_TYPE_MAP = {
    'xr_int': 'int',
    'xr_float': 'float',
    'xr_bool': 'bool',
    'xrs_string_value_c': 'string',
}

# Pattern to match type class definitions (for builtin types like Array, String)
TYPE_CLASS_PATTERN = re.compile(
    r'// @type\s+(\w+)'
)

# Pattern to match module declarations
MODULE_PATTERN = re.compile(
    r'// @module\s+(\w+)'
)

# Pattern to match handle type declarations
# e.g., // @handle Connection { const fd: int, const type: string, const tls: bool }
HANDLE_PATTERN = re.compile(
    r'// @handle\s+(\w+)\s*\{([^}]+)\}'
)

# Pattern to parse individual handle fields
HANDLE_FIELD_PATTERN = re.compile(
    r'(const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*'
    r'([A-Za-z_][A-Za-z0-9_]*(?:<[^,{}]+>)?\??)'
)


def parse_handle_fields(fields_str):
    """Parse handle field declarations."""
    fields = []
    for match in HANDLE_FIELD_PATTERN.finditer(fields_str):
        is_const = match.group(1) is not None
        name = match.group(2)
        type_str = match.group(3)
        fields.append({
            'name': name,
            'type': type_str,
            'is_const': is_const,
        })
    return fields


def scan_file(filepath):
    """Scan a C file for builtin definitions.

    When a file contains both ``// @module`` and ``// @type`` annotations,
    XR_DEFINE_BUILTIN entries *before* the ``@type`` marker are module-level
    functions, and entries *after* it are instance methods on the type.
    """
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Warning: Cannot read {filepath}: {e}", file=sys.stderr)
        return None

    # Collapse C adjacent string literals so multi-line doc strings
    # in XR_DEFINE_BUILTIN are matched by the single-string regex.
    content = _join_adjacent_strings(content)

    result = {
        'type': None,          # @type annotation (for builtin types)
        'module': None,        # @module annotation (for C modules)
        'handles': [],         # @handle annotations
        'methods': [],         # XR_DEFINE_BUILTIN entries (module-level functions)
        'type_methods': [],    # XR_DEFINE_BUILTIN entries after @type (instance methods)
        'handle_methods': {},  # TypeName -> [methods] from "TypeName.method" convention
        'constants': [],       # xr_module_add_export(...) entries (typed constants)
    }

    # Find @type annotation (record position for method splitting)
    type_pos = None
    for match in TYPE_CLASS_PATTERN.finditer(content):
        result['type'] = match.group(1)
        type_pos = match.start()

    # Find @module annotation
    for match in MODULE_PATTERN.finditer(content):
        result['module'] = match.group(1)

    # Find @handle annotations
    for match in HANDLE_PATTERN.finditer(content):
        handle_name = match.group(1)
        fields = parse_handle_fields(match.group(2))
        result['handles'].append({
            'name': handle_name,
            'fields': fields,
        })

    # Find XR_DEFINE_BUILTIN entries.
    # Names containing a dot (e.g. "SqliteDB.exec") denote instance methods
    # on a handle type; they are grouped under handle_methods[TypeName].
    # When both @module and @type are present, entries after @type go into
    # type_methods (instance methods) rather than methods (module functions).
    for match in METHOD_PATTERN.finditer(content):
        func_name, method_name, signature, doc = match.groups()
        if '.' in method_name:
            type_name, meth = method_name.split('.', 1)
            result['handle_methods'].setdefault(type_name, []).append({
                'func': func_name,
                'name': meth,
                'signature': signature,
                'doc': doc,
            })
        else:
            entry = {
                'func': func_name,
                'name': method_name,
                'signature': signature,
                'doc': doc,
            }
            if type_pos is not None and match.start() > type_pos:
                result['type_methods'].append(entry)
            else:
                result['methods'].append(entry)

    # Find xr_module_add_export(...) constant registrations and surface them
    # as analyzer-visible members. These are *not* methods, so signature is
    # stored as ": <type>" (matches the convention already used by hand-
    # maintained entries) and is_method is forced to false at emission time.
    seen = set()
    for match in EXPORT_PATTERN.finditer(content):
        const_name, ctor = match.group(1), match.group(2)
        if const_name in seen:
            continue
        seen.add(const_name)
        const_type = EXPORT_TYPE_MAP.get(ctor)
        if not const_type:
            continue
        result['constants'].append({
            'name': const_name,
            'signature': f": {const_type}",
            # No reliable way to derive a meaningful sentence from C source,
            # so emit an empty doc string — analyzer/LSP behaviour is the
            # same; only the in-IDE hover text is empty.
            'doc': '',
        })

    if (not result['methods'] and not result['type_methods']
            and not result['handles']
            and not result['constants'] and not result['handle_methods']):
        return None

    return result


def load_def_module_methods():
    """Load migrated module functions from stdlib/defs/*.def."""
    sys.path.insert(0, str(STDLIBGEN_TOOL_DIR))
    try:
        from stdlibgen import parse_defs
    except Exception as e:
        raise SystemExit(f"Error: cannot import stdlibgen parser: {e}") from e
    finally:
        try:
            sys.path.remove(str(STDLIBGEN_TOOL_DIR))
        except ValueError:
            pass

    modules = {}
    for entry in parse_defs(PROJECT_ROOT):
        modules.setdefault(entry.module, []).append({
            'func': entry.vm,
            'name': entry.name,
            'signature': entry.signature,
            'doc': entry.doc,
        })
    return modules


def load_def_module_constants():
    """Load migrated module constants from stdlib/defs/*.def."""
    sys.path.insert(0, str(STDLIBGEN_TOOL_DIR))
    try:
        from stdlibgen import parse_constants
    except Exception as e:
        raise SystemExit(f"Error: cannot import stdlibgen parser: {e}") from e
    finally:
        try:
            sys.path.remove(str(STDLIBGEN_TOOL_DIR))
        except ValueError:
            pass

    modules = {}
    for entry in parse_constants(PROJECT_ROOT):
        modules.setdefault(entry.module, []).append({
            'name': entry.name,
            'signature': entry.signature,
            'doc': entry.doc,
        })
    return modules


def load_def_module_handles():
    """Load migrated handle declarations from stdlib/defs/*.def."""
    sys.path.insert(0, str(STDLIBGEN_TOOL_DIR))
    try:
        from stdlibgen import parse_handles
    except Exception as e:
        raise SystemExit(f"Error: cannot import stdlibgen parser: {e}") from e
    finally:
        try:
            sys.path.remove(str(STDLIBGEN_TOOL_DIR))
        except ValueError:
            pass

    modules = {}
    for entry in parse_handles(PROJECT_ROOT):
        modules.setdefault(entry.module, []).append({
            'name': entry.name,
            'fields': [
                {
                    'name': field.name,
                    'type': field.type,
                    'is_const': field.is_const,
                }
                for field in entry.fields
            ],
        })
    return modules


def merge_def_module_methods(module_results, def_methods):
    """Overlay .def methods onto C-scanned module declarations.

    C annotations still own handles and modules not yet migrated.
    For migrated functions, .def metadata is the primary source of signature
    and documentation used by analyzer/LSP/MCP-facing builtin tables.
    """
    replaced = 0
    added = 0
    for mod_name, methods in sorted(def_methods.items()):
        mod_data = module_results.setdefault(mod_name, {
            'handles': [],
            'methods': [],
            'handle_methods': {},
            'constants': [],
        })
        by_name = {}
        for idx, method in enumerate(mod_data.get('methods', [])):
            by_name.setdefault(method['name'], idx)
        for method in methods:
            idx = by_name.get(method['name'])
            if idx is None:
                mod_data.setdefault('methods', []).append(method)
                by_name[method['name']] = len(mod_data['methods']) - 1
                added += 1
            else:
                mod_data['methods'][idx] = method
                replaced += 1
    return replaced, added


def merge_def_module_constants(module_results, def_constants):
    """Overlay .def constants onto C-scanned module declarations."""
    replaced = 0
    added = 0
    for mod_name, constants in sorted(def_constants.items()):
        mod_data = module_results.setdefault(mod_name, {
            'handles': [],
            'methods': [],
            'handle_methods': {},
            'constants': [],
        })
        by_name = {}
        for idx, const in enumerate(mod_data.get('constants', [])):
            by_name.setdefault(const['name'], idx)
        for const in constants:
            idx = by_name.get(const['name'])
            if idx is None:
                mod_data.setdefault('constants', []).append(const)
                by_name[const['name']] = len(mod_data['constants']) - 1
                added += 1
            else:
                mod_data['constants'][idx] = const
                replaced += 1
    return replaced, added


def merge_def_module_handles(module_results, def_handles):
    """Overlay .def handles onto C-scanned module declarations."""
    replaced = 0
    added = 0
    for mod_name, handles in sorted(def_handles.items()):
        mod_data = module_results.setdefault(mod_name, {
            'handles': [],
            'methods': [],
            'handle_methods': {},
            'constants': [],
        })
        by_name = {}
        for idx, handle in enumerate(mod_data.get('handles', [])):
            by_name.setdefault(handle['name'], idx)
        for handle in handles:
            idx = by_name.get(handle['name'])
            if idx is None:
                mod_data.setdefault('handles', []).append(handle)
                by_name[handle['name']] = len(mod_data['handles']) - 1
                added += 1
            else:
                mod_data['handles'][idx] = handle
                replaced += 1
    return replaced, added


def c_string(value):
    """Escape a Python string as a C string literal body."""
    if value is None:
        value = ""
    return (str(value)
            .replace('\\', '\\\\')
            .replace('"', '\\"')
            .replace('\n', '\\n')
            .replace('\r', '\\r')
            .replace('\t', '\\t'))


def c_ident(value):
    """Return a stable C identifier fragment for generated symbol names."""
    ident = re.sub(r'[^0-9A-Za-z_]', '_', value).lower()
    if not ident:
        return "unnamed"
    if ident[0].isdigit():
        ident = "_" + ident
    return ident


def lsp_signature(signature):
    """Convert analyzer/member signatures to the display form LSP expects."""
    sig = (signature or "").strip()
    if not sig:
        return ""
    if sig.startswith(":"):
        return sig[1:].strip()
    if sig.startswith("fn"):
        return sig
    if sig.startswith("("):
        return "fn" + sig
    return sig


def lsp_kind_for_member(member):
    signature = (member.get('signature') or "").strip()
    if member.get('is_method'):
        return "XLSP_SYM_FUNCTION"
    if signature.startswith(":"):
        return "XLSP_SYM_CONSTANT"
    if '(' in signature:
        return "XLSP_SYM_FUNCTION"
    return "XLSP_SYM_PROPERTY"


def generate_header(type_results, module_results):
    """Generate the embedded header file content."""
    lines = [
        "/*",
        " * AUTO-GENERATED FILE - DO NOT EDIT",
        " * Generated by scripts/gen_stdlib_types.py",
        " * Sources: stdlib definition files + C annotations fallback",
        " *",
        " * xanalyzer_builtins_generated.h - Generated builtin type declarations",
        " */",
        "",
        "#ifndef XANALYZER_BUILTINS_GENERATED_H",
        "#define XANALYZER_BUILTINS_GENERATED_H",
        "",
        "#include \"xanalyzer_builtins.h\"",
        "",
        "/* clang-format off */",
        "",
    ]

    # Builtin type members (Array, String, etc.)
    if type_results:
        lines.append("// ======== Builtin Type Members ========")
        lines.append("")
        for type_name, methods in sorted(type_results.items()):
            if not methods:
                continue
            lines.append(f"// {type_name} methods")
            lines.append(f"static const XaBuiltinMember g_gen_{type_name.lower()}_members[] = {{")
            for m in methods:
                is_method = "true" if '(' in m['signature'] else "false"
                lines.append(f'    {{"{m["name"]}", "{m["signature"]}", "{m["doc"]}", {is_method}, false}},')
            lines.append("};")
            lines.append(f"#define GEN_{type_name.upper()}_MEMBER_COUNT {len(methods)}")
            lines.append("")

    # C module declarations (net, ws, http, etc.)
    if module_results:
        lines.append("// ======== C Module Declarations ========")
        lines.append("")

        for mod_name, mod_data in sorted(module_results.items()):
            # Handle types
            for handle in mod_data.get('handles', []):
                var_name = f"g_gen_{mod_name}_{handle['name'].lower()}_fields"
                lines.append(f"// {mod_name}.{handle['name']} handle fields")
                lines.append(f"static const XaBuiltinHandleField {var_name}[] = {{")
                for f in handle['fields']:
                    is_const = "true" if f['is_const'] else "false"
                    lines.append(f'    {{"{f["name"]}", "{f["type"]}", {is_const}}},')
                lines.append("};")
                lines.append("")

            # Handle array
            if mod_data.get('handles'):
                lines.append(f"static const XaBuiltinHandle g_gen_{mod_name}_handles[] = {{")
                for handle in mod_data['handles']:
                    var_name = f"g_gen_{mod_name}_{handle['name'].lower()}_fields"
                    lines.append(f'    {{"{handle["name"]}", {var_name}, {len(handle["fields"])}, NULL, 0}},')
                lines.append("};")
                lines.append(f"#define GEN_{mod_name.upper()}_HANDLE_COUNT {len(mod_data['handles'])}")
                lines.append("")

            # Function declarations.
            #
            # Methods (functions with signatures) come first, followed by
            # typed constants surfaced from xr_module_add_export(). Both
            # share the XaBuiltinMember table so the analyzer / LSP can
            # resolve `mod.foo` uniformly regardless of whether `foo` is a
            # callable or a constant. The is_method bit is what drives the
            # downstream behavioural fork.
            method_entries = list(mod_data.get('methods', []))
            constant_entries = list(mod_data.get('constants', []))
            total = len(method_entries) + len(constant_entries)
            if total > 0:
                lines.append(f"// {mod_name} module functions")
                lines.append(f"static const XaBuiltinMember g_gen_{mod_name}_functions[] = {{")
                for m in method_entries:
                    is_method = "true" if '(' in m['signature'] else "false"
                    lines.append(f'    {{"{m["name"]}", "{m["signature"]}", "{m["doc"]}", {is_method}, false}},')
                if constant_entries:
                    lines.append(f"    // Module constants (is_method=false)")
                    for c in constant_entries:
                        lines.append(f'    {{"{c["name"]}", "{c["signature"]}", "{c["doc"]}", false, false}},')
                lines.append("};")
                lines.append(f"#define GEN_{mod_name.upper()}_FUNCTION_COUNT {total}")
                lines.append("")

        # Module registry
        lines.append("// Module registry")
        lines.append("static const XaBuiltinModule g_gen_builtin_modules[] = {")
        for mod_name, mod_data in sorted(module_results.items()):
            # A module emits a function table when it has methods OR typed
            # constants — the latter alone (e.g. an "endian" module that
            # only exports LE/BE) is enough to need a non-NULL slot.
            has_function_slot = bool(mod_data.get('methods') or mod_data.get('constants'))
            func_ref = f"g_gen_{mod_name}_functions" if has_function_slot else "NULL"
            func_count = f"GEN_{mod_name.upper()}_FUNCTION_COUNT" if has_function_slot else "0"
            handle_ref = f"g_gen_{mod_name}_handles" if mod_data.get('handles') else "NULL"
            handle_count = f"GEN_{mod_name.upper()}_HANDLE_COUNT" if mod_data.get('handles') else "0"
            lines.append(f'    {{"{mod_name}", {func_ref}, {func_count}, {handle_ref}, {handle_count}}},')
        lines.append("};")
        lines.append(f"#define GEN_BUILTIN_MODULE_COUNT {len(module_results)}")
        lines.append("")

    lines.append("/* clang-format on */")
    lines.append("")
    lines.append("#endif  // XANALYZER_BUILTINS_GENERATED_H")
    lines.append("")

    return "\n".join(lines)


def generate_lsp_include(module_results):
    """Generate LSP stdlib module declarations from the same module metadata."""
    lines = [
        "/*",
        " * AUTO-GENERATED FILE - DO NOT EDIT",
        " * Generated by scripts/gen_stdlib_types.py",
        " * Sources: stdlib definition files + C annotations fallback",
        " *",
        " * xlsp_stdlib_generated.inc - Generated LSP stdlib declarations",
        " */",
        "",
        "/* clang-format off */",
        "",
    ]

    emitted_modules = []
    for mod_name, mod_data in sorted(module_results.items()):
        symbols = []
        for m in mod_data.get('methods', []):
            entry = dict(m)
            entry['is_method'] = True
            symbols.append(entry)
        for c in mod_data.get('constants', []):
            entry = dict(c)
            entry['is_method'] = False
            symbols.append(entry)
        for handle in mod_data.get('handles', []):
            symbols.append({
                'name': handle['name'],
                'signature': f"type {handle['name']}",
                'doc': "Native handle type",
                'lsp_kind': "XLSP_SYM_CLASS",
            })

        if not symbols:
            continue

        mod_ident = c_ident(mod_name)
        array_name = f"g_xlsp_stdlib_{mod_ident}_symbols"
        count_name = f"GEN_XLSP_STDLIB_{mod_ident.upper()}_SYMBOL_COUNT"
        lines.append(f"// {mod_name} module symbols")
        lines.append(f"static const XlspSymbolInfo {array_name}[] = {{")
        for sym in symbols:
            kind = sym.get('lsp_kind') or lsp_kind_for_member(sym)
            sig = lsp_signature(sym.get('signature'))
            doc = sym.get('doc') or ""
            lines.append(
                f'    {{"{c_string(sym["name"])}", {kind}, "{c_string(sig)}", '
                f'"{c_string(doc)}", NULL, 0}},')
        lines.append("};")
        lines.append(f"#define {count_name} {len(symbols)}")
        lines.append("")
        emitted_modules.append((mod_name, array_name, count_name))

    lines.append("static const XlspModuleInfo g_xlsp_stdlib_modules[] = {")
    for mod_name, array_name, count_name in emitted_modules:
        module_doc = f"{mod_name} standard library module"
        lines.append(
            f'    {{"{c_string(mod_name)}", "{c_string(module_doc)}", '
            f'{array_name}, {count_name}}},')
    lines.append("};")
    lines.append("static const int g_xlsp_stdlib_module_count =")
    lines.append("    sizeof(g_xlsp_stdlib_modules) / sizeof(g_xlsp_stdlib_modules[0]);")
    lines.append("")
    lines.append("/* clang-format on */")
    lines.append("")
    return "\n".join(lines)


def generate_xrd(mod_data):
    """Generate .xrd declaration file content."""
    lines = []
    mod_name = mod_data.get('module', 'unknown')
    lines.append(f"// {mod_name}.xrd (auto-generated from C source by gen_stdlib_types.py)")
    lines.append("")

    # Handle types
    for handle in mod_data.get('handles', []):
        fields_str = ", ".join(
            f"{'const ' if f['is_const'] else ''}{f['name']}: {f['type']}"
            for f in handle['fields']
        )
        lines.append(f"type {handle['name']} = {{ {fields_str} }}")
    if mod_data.get('handles'):
        lines.append("")

    # Functions
    for m in mod_data.get('methods', []):
        lines.append(f"export fn {m['name']}{m['signature']}")

    # Typed constants (registered via xr_module_add_export at runtime).
    # Surface as `export const NAME: type` in the .xrd so downstream
    # tooling (analyzer / LSP / docs generator) sees them as values
    # rather than callables.
    for c in mod_data.get('constants', []):
        # signature is stored as ": type"; strip the leading ": " to
        # produce a clean `: type` annotation in the output.
        type_name = c['signature'].lstrip(': ').strip()
        lines.append(f"export const {c['name']}: {type_name}")

    # Handle instance methods (from "TypeName.method" convention)
    handle_methods = mod_data.get('handle_methods', {})
    if handle_methods:
        lines.append("")
        for type_name in sorted(handle_methods.keys()):
            for m in handle_methods[type_name]:
                lines.append(f"fn {type_name}.{m['name']}{m['signature']}")

    lines.append("")
    return "\n".join(lines)


def check_file(path, content, label):
    if not path.exists():
        print(f"missing generated {label}: {path}", file=sys.stderr)
        return 1
    current = path.read_text(encoding='utf-8')
    if current == content:
        return 0
    print(f"stale generated {label}: {path}", file=sys.stderr)
    diff = difflib.unified_diff(
        current.splitlines(),
        content.splitlines(),
        fromfile=str(path),
        tofile=f"{path} (regenerated)",
        lineterm="",
    )
    for line in list(diff)[:160]:
        print(line, file=sys.stderr)
    return 1


def check_outputs(analyzer_content, lsp_content):
    status = 0
    status |= check_file(OUTPUT_FILE, analyzer_content, "analyzer builtins")
    status |= check_file(LSP_OUTPUT_FILE, lsp_content, "LSP stdlib table")
    return status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xrd", metavar="FILE", help="generate .xrd declarations from one C file")
    parser.add_argument("--check", action="store_true", help="verify generated embed header is current")
    args = parser.parse_args()

    # --xrd mode: generate .xrd from a single C file
    if args.xrd:
        if args.check:
            print("Error: --check is only valid in embed mode", file=sys.stderr)
            return 1
        filepath = Path(args.xrd)
        if not filepath.exists():
            print(f"Error: File not found: {filepath}", file=sys.stderr)
            return 1

        result = scan_file(filepath)
        if not result:
            print(f"Error: No XR_DEFINE_BUILTIN or xr_module_add_export found "
                  f"in {filepath}", file=sys.stderr)
            return 1

        mod_data = {
            'module': result['module'] or filepath.stem,
            'handles': result['handles'],
            'methods': result['methods'],
            'handle_methods': result['handle_methods'],
            'constants': result['constants'],
        }
        print(generate_xrd(mod_data))
        return 0

    # --embed mode (default): scan all stdlib sources and generate header
    print("Scanning stdlib sources for builtin definitions...", file=sys.stderr)

    type_results = {}    # type_name -> [methods]
    module_results = {}  # module_name -> {handles, methods}

    for dir_path in STDLIB_DIRS:
        if not dir_path.exists():
            continue

        for filepath in dir_path.rglob("*.c"):
            result = scan_file(filepath)
            if not result:
                continue

            if result['module']:
                # C module (net, ws, http, etc.)
                mod_name = result['module']
                module_results[mod_name] = {
                    'handles': result['handles'],
                    'methods': result['methods'],
                    'handle_methods': result['handle_methods'],
                    'constants': result['constants'],
                }
                hm_count = sum(len(v) for v in result['handle_methods'].values())
                print(f"  Module '{mod_name}': {len(result['methods'])} functions, "
                      f"{hm_count} handle methods, "
                      f"{len(result['constants'])} constants, "
                      f"{len(result['handles'])} handles in {filepath.name}", file=sys.stderr)

            # When a file has both @module and @type, instance methods
            # (after @type) are split into type_results separately.
            if result['type'] and result['type_methods']:
                type_name = result['type']
                if type_name not in type_results:
                    type_results[type_name] = []
                type_results[type_name].extend(result['type_methods'])
                print(f"  Type '{type_name}': {len(result['type_methods'])} methods in "
                      f"{filepath.name}", file=sys.stderr)

            if result['type'] and not result['module']:
                # Pure type file (Array, String, etc.) — no @module
                type_name = result['type']
                if type_name not in type_results:
                    type_results[type_name] = []
                type_results[type_name].extend(result['methods'])
                if result['methods']:
                    print(f"  Type '{type_name}': {len(result['methods'])} methods in "
                          f"{filepath.name}", file=sys.stderr)

    def_methods = load_def_module_methods()
    replaced, added = merge_def_module_methods(module_results, def_methods)
    def_constants = load_def_module_constants()
    const_replaced, const_added = merge_def_module_constants(module_results, def_constants)
    def_handles = load_def_module_handles()
    handle_replaced, handle_added = merge_def_module_handles(module_results, def_handles)

    total_types = len(type_results)
    total_modules = len(module_results)
    total_methods = sum(len(m) for m in type_results.values())
    total_functions = sum(len(m['methods']) for m in module_results.values())
    total_constants = sum(len(m.get('constants', [])) for m in module_results.values())

    print(f"\nTotal: {total_methods} type methods across {total_types} types, "
          f"{total_functions} module functions and {total_constants} constants "
          f"across {total_modules} modules", file=sys.stderr)
    print(f"Def overlay: {replaced} functions replaced, {added} functions added "
          f"from stdlib/defs/*.def", file=sys.stderr)
    print(f"Def constants overlay: {const_replaced} constants replaced, "
          f"{const_added} constants added from stdlib/defs/*.def", file=sys.stderr)
    print(f"Def handles overlay: {handle_replaced} handles replaced, "
          f"{handle_added} handles added from stdlib/defs/*.def", file=sys.stderr)

    # Generate embed outputs
    analyzer_content = generate_header(type_results, module_results)
    lsp_content = generate_lsp_include(module_results)

    if args.check:
        return check_outputs(analyzer_content, lsp_content)

    # Write outputs
    OUTPUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write(analyzer_content)
    LSP_OUTPUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(LSP_OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write(lsp_content)

    print(f"\nGenerated: {OUTPUT_FILE}", file=sys.stderr)
    print(f"Generated: {LSP_OUTPUT_FILE}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
