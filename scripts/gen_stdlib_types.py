#!/usr/bin/env python3
"""
xray - Lightweight typed scripting with native concurrency
https://www.xray-lang.org

Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
Licensed under the MIT License

gen_stdlib_types.py - Generate analyzer builtin type declarations from C sources

KEY CONCEPT:
  Parses C source files to extract builtin method/function definitions and
  generates corresponding type declarations for the analyzer.

  Supports two output modes:
    --embed (default): Generate xanalyzer_builtins_generated.h (for stdlib)
    --xrd <file.c>:    Generate .xrd declaration file (for third-party modules)

USAGE:
  python3 scripts/gen_stdlib_types.py                    # embed mode
  python3 scripts/gen_stdlib_types.py --xrd my_db.c      # xrd mode

This ensures type declarations stay in sync with runtime implementations.
"""

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

# Output file (embed mode)
OUTPUT_FILE = PROJECT_ROOT / "src" / "frontend" / "analyzer" / "xanalyzer_builtins_generated.h"

# Pattern to match method/function definitions in C
# e.g., XR_DEFINE_BUILTIN(array_push, "push", "(item: T): int", "Push item to end")
METHOD_PATTERN = re.compile(
    r'XR_DEFINE_BUILTIN\s*\(\s*'
    r'(\w+)\s*,\s*'           # C function name
    r'"([^"]+)"\s*,\s*'       # Method/function name
    r'"([^"]+)"\s*,\s*'       # Signature
    r'"([^"]+)"\s*\)'         # Documentation
)

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
    r'(const\s+)?(\w+)\s*:\s*(\w+)'
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
    """Scan a C file for builtin definitions."""
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Warning: Cannot read {filepath}: {e}", file=sys.stderr)
        return None

    result = {
        'type': None,       # @type annotation (for builtin types)
        'module': None,     # @module annotation (for C modules)
        'handles': [],      # @handle annotations
        'methods': [],      # XR_DEFINE_BUILTIN entries (functions/methods)
        'constants': [],    # xr_module_add_export(...) entries (typed constants)
    }

    # Find @type annotation
    for match in TYPE_CLASS_PATTERN.finditer(content):
        result['type'] = match.group(1)

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

    # Find XR_DEFINE_BUILTIN entries
    for match in METHOD_PATTERN.finditer(content):
        func_name, method_name, signature, doc = match.groups()
        result['methods'].append({
            'func': func_name,
            'name': method_name,
            'signature': signature,
            'doc': doc,
        })

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

    if not result['methods'] and not result['handles'] and not result['constants']:
        return None

    return result


def generate_header(type_results, module_results):
    """Generate the embedded header file content."""
    lines = [
        "/*",
        " * AUTO-GENERATED FILE - DO NOT EDIT",
        " * Generated by scripts/gen_stdlib_types.py",
        " *",
        " * xanalyzer_builtins_generated.h - Generated builtin type declarations",
        " */",
        "",
        "#ifndef XANALYZER_BUILTINS_GENERATED_H",
        "#define XANALYZER_BUILTINS_GENERATED_H",
        "",
        "#include \"xanalyzer_builtins.h\"",
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
                    lines.append(f'    {{"{handle["name"]}", {var_name}, {len(handle["fields"])}}},')
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

    lines.append("#endif // XANALYZER_BUILTINS_GENERATED_H")
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

    lines.append("")
    return "\n".join(lines)


def main():
    args = sys.argv[1:]

    # --xrd mode: generate .xrd from a single C file
    if len(args) >= 2 and args[0] == '--xrd':
        filepath = Path(args[1])
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
                    'constants': result['constants'],
                }
                print(f"  Module '{mod_name}': {len(result['methods'])} functions, "
                      f"{len(result['constants'])} constants, "
                      f"{len(result['handles'])} handles in {filepath.name}", file=sys.stderr)
            elif result['type']:
                # Builtin type (Array, String, etc.)
                type_name = result['type']
                if type_name not in type_results:
                    type_results[type_name] = []
                type_results[type_name].extend(result['methods'])
                print(f"  Type '{type_name}': {len(result['methods'])} methods in {filepath.name}",
                      file=sys.stderr)

    total_types = len(type_results)
    total_modules = len(module_results)
    total_methods = sum(len(m) for m in type_results.values())
    total_functions = sum(len(m['methods']) for m in module_results.values())
    total_constants = sum(len(m.get('constants', [])) for m in module_results.values())

    print(f"\nTotal: {total_methods} type methods across {total_types} types, "
          f"{total_functions} module functions and {total_constants} constants "
          f"across {total_modules} modules", file=sys.stderr)

    # Generate header
    content = generate_header(type_results, module_results)

    # Write output
    OUTPUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write(content)

    print(f"\nGenerated: {OUTPUT_FILE}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
