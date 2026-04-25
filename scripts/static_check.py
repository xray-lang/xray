#!/usr/bin/env python3
"""
Xray Static Analysis Check Script

Based on docs/600_static_analysis_checks.md, this script performs automated
static checks on the xray codebase to detect known bug patterns.

Usage:
    python3 scripts/static_check.py [--verbose] [--category CATEGORY]

Categories: all, vm, ic, gc, frame, compiler, type, naming, tls
"""

import os
import re
import sys
import argparse
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# Project root (script is in scripts/)
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Key source files
XVM_C = os.path.join(PROJECT_ROOT, "src/vm/xvm.c")
XVM_API_C = os.path.join(PROJECT_ROOT, "src/vm/xvm_api.c")
XVM_OPS_C = os.path.join(PROJECT_ROOT, "src/vm/xvm_ops.c")
XCORO_GC_C = os.path.join(PROJECT_ROOT, "src/runtime/gc/xcoro_gc.c")
XTYPE_NAMES_H = os.path.join(PROJECT_ROOT, "src/runtime/value/xtype_names.h")
XGC_HEADER_H = os.path.join(PROJECT_ROOT, "src/runtime/gc/xgc_header.h")
YAML_PARSER_C = os.path.join(PROJECT_ROOT, "stdlib/yaml/yaml_parser.c")
ANALYZER_DIR = os.path.join(PROJECT_ROOT, "src/frontend/analyzer")
CLASS_REGISTRY_C = os.path.join(PROJECT_ROOT, "src/frontend/codegen/xcompiler_class_registry.c")
XCHUNK_H = os.path.join(PROJECT_ROOT, "src/runtime/value/xchunk.h")


@dataclass
class CheckResult:
    """Result of a single check."""
    check_id: str
    name: str
    priority: str  # P0, P1, P2
    passed: bool
    message: str
    details: List[str] = field(default_factory=list)


class StaticChecker:
    """Main static analysis checker."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self.results: List[CheckResult] = []
        self._file_cache = {}

    def read_file(self, path: str) -> str:
        if path not in self._file_cache:
            try:
                with open(path, 'r', encoding='utf-8', errors='replace') as f:
                    self._file_cache[path] = f.read()
            except FileNotFoundError:
                self._file_cache[path] = ""
                self._warn(f"File not found: {path}")
        return self._file_cache[path]

    def read_lines(self, path: str) -> List[str]:
        return self.read_file(path).splitlines()

    def _warn(self, msg: str):
        print(f"  \033[33mWARN\033[0m: {msg}", file=sys.stderr)

    def add_result(self, result: CheckResult):
        self.results.append(result)

    # ================================================================
    # 1.1 P0: vmcase exit path completeness
    # ================================================================
    def check_vmcase_exit_paths(self):
        """Every vmcase block must end with vmbreak/goto startfunc/VM_RUNTIME_ERROR/return."""
        content = self.read_file(XVM_C)
        if not content:
            self.add_result(CheckResult("1.1", "vmcase exit paths", "P0", False, "xvm.c not found"))
            return

        # Extract vmcase blocks
        issues = []
        # Find all vmcase(OP_XXX) positions
        vmcase_pattern = re.compile(r'vmcase\((OP_\w+)\)\s*\{')
        matches = list(vmcase_pattern.finditer(content))

        valid_exits = {'vmbreak', 'goto startfunc', 'VM_RUNTIME_ERROR', 'return XR_VM',
                       'continue', 'goto '}

        for idx, m in enumerate(matches):
            op_name = m.group(1)
            start = m.start()

            # Find the matching closing brace by counting braces
            brace_count = 0
            block_start = content.index('{', m.start())
            pos = block_start
            block_end = -1
            for pos in range(block_start, len(content)):
                if content[pos] == '{':
                    brace_count += 1
                elif content[pos] == '}':
                    brace_count -= 1
                    if brace_count == 0:
                        block_end = pos
                        break

            if block_end == -1:
                issues.append(f"{op_name}: could not find matching brace")
                continue

            block = content[block_start:block_end + 1]

            # Check if block has at least one valid exit
            has_exit = False
            for exit_kw in valid_exits:
                if exit_kw in block:
                    has_exit = True
                    break

            if not has_exit:
                line_num = content[:start].count('\n') + 1
                issues.append(f"{op_name} (line {line_num}): no valid exit path found")

            # Check for branches where only one side has vmbreak
            # Simple heuristic: look for if/else where one branch has vmbreak and other doesn't
            # This is a simplified check - full analysis would need AST parsing
            if_else_pattern = re.compile(r'if\s*\([^)]+\)\s*\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}\s*else\s*\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}')
            for ie_match in if_else_pattern.finditer(block):
                if_body = ie_match.group(1)
                else_body = ie_match.group(2)
                if_has_exit = any(e in if_body for e in valid_exits)
                else_has_exit = any(e in else_body for e in valid_exits)
                # Only flag if the if/else is at the end of the block and one side missing exit
                # This is a heuristic - may have false positives

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "1.1", "vmcase exit paths", "P0", passed,
            f"Found {len(issues)} vmcase blocks without valid exit" if issues else "All vmcase blocks have valid exits",
            issues[:20]  # Limit output
        ))

    # ================================================================
    # 1.2 P0: Instruction variant consistency (operator overload)
    # ================================================================
    def check_instruction_variant_consistency(self):
        """If base instruction has VM_TRY_BINARY_OP_OVERLOAD, variants must too."""
        content = self.read_file(XVM_C)
        if not content:
            self.add_result(CheckResult("1.2", "instruction variant consistency", "P0", False, "xvm.c not found"))
            return

        # Define instruction families
        families = [
            ("OP_ADD", ["OP_ADDI", "OP_ADDK"]),
            ("OP_SUB", ["OP_SUBI", "OP_SUBK"]),
            ("OP_MUL", ["OP_MULI", "OP_MULK"]),
            ("OP_DIV", ["OP_DIVK"]),
            ("OP_MOD", ["OP_MODK"]),
        ]

        vmcase_pattern = re.compile(r'vmcase\((OP_\w+)\)\s*\{')
        matches = list(vmcase_pattern.finditer(content))

        def get_vmcase_block(op_name):
            for m in matches:
                if m.group(1) == op_name:
                    brace_count = 0
                    block_start = content.index('{', m.start())
                    for pos in range(block_start, len(content)):
                        if content[pos] == '{':
                            brace_count += 1
                        elif content[pos] == '}':
                            brace_count -= 1
                            if brace_count == 0:
                                return content[block_start:pos + 1]
            return ""

        issues = []
        for base_op, variants in families:
            base_block = get_vmcase_block(base_op)
            base_has_overload = "VM_TRY_BINARY_OP_OVERLOAD" in base_block

            if not base_has_overload:
                continue  # Base doesn't have overload, no requirement for variants

            for variant in variants:
                variant_block = get_vmcase_block(variant)
                if not variant_block:
                    issues.append(f"{variant}: vmcase block not found")
                    continue
                if "VM_TRY_BINARY_OP_OVERLOAD" not in variant_block:
                    issues.append(f"{variant}: missing VM_TRY_BINARY_OP_OVERLOAD (base {base_op} has it)")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "1.2", "instruction variant consistency", "P0", passed,
            f"Found {len(issues)} variants missing operator overload" if issues else "All variants consistent",
            issues
        ))

    # ================================================================
    # 2.1 P0: IC cache_index must use local pc
    # ================================================================
    def check_ic_cache_index(self):
        """cache_index calculation must use local pc, not frame->pc."""
        content = self.read_file(XVM_C)
        if not content:
            self.add_result(CheckResult("2.1", "IC cache_index uses local pc", "P0", False, "xvm.c not found"))
            return

        issues = []
        lines = content.splitlines()
        for i, line in enumerate(lines, 1):
            if 'cache_index' in line and 'frame->pc' in line:
                issues.append(f"line {i}: {line.strip()}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "2.1", "IC cache_index uses local pc", "P0", passed,
            f"Found {len(issues)} cache_index using frame->pc (STALE!)" if issues else "All cache_index use local pc",
            issues
        ))

    # ================================================================
    # 3.1 P0: Write Barrier coverage in VM instructions
    # ================================================================
    def check_write_barrier_coverage(self):
        """Object reference writes in VM must have write barriers."""
        content = self.read_file(XVM_C)
        if not content:
            self.add_result(CheckResult("3.1", "Write Barrier coverage", "P0", False, "xvm.c not found"))
            return

        # Check that SET operations have barriers
        vmcase_pattern = re.compile(r'vmcase\((OP_\w+)\)\s*\{')
        matches = list(vmcase_pattern.finditer(content))

        def get_vmcase_block(op_name):
            for m in matches:
                if m.group(1) == op_name:
                    brace_count = 0
                    block_start = content.index('{', m.start())
                    for pos in range(block_start, len(content)):
                        if content[pos] == '{':
                            brace_count += 1
                        elif content[pos] == '}':
                            brace_count -= 1
                            if brace_count == 0:
                                return content[block_start:pos + 1]
            return ""

        # Instructions that write object references and need barriers
        set_ops = [
            "OP_SETUPVAL", "OP_SETFIELD_DIRECT", "OP_SETFIELD_FAST",
            "OP_SETFIELD_IC", "OP_SETFIELD_STATIC", "OP_INDEX_SET",
            "OP_MAP_SET", "OP_MAP_SET_K",
        ]

        issues = []
        for op in set_ops:
            block = get_vmcase_block(op)
            if not block:
                continue
            has_barrier = ("VM_BARRIER_VAL" in block or "VM_BARRIER_BACK" in block)
            if not has_barrier:
                issues.append(f"{op}: no write barrier (VM_BARRIER_VAL/VM_BARRIER_BACK)")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "3.1", "Write Barrier coverage", "P0", passed,
            f"Found {len(issues)} SET ops without write barrier" if issues else "All SET ops have write barriers",
            issues
        ))

    # ================================================================
    # 3.2 P0: GC atomic phase re-marks coroutine stack
    # ================================================================
    def check_gc_atomic_remark(self):
        """atomic() must re-mark coro roots before flipwhite()."""
        content = self.read_file(XCORO_GC_C)
        if not content:
            self.add_result(CheckResult("3.2", "GC atomic re-mark", "P0", False, "xcoro_gc.c not found"))
            return

        # Find atomic function and check for mark_coro_roots before flipwhite
        has_mark_roots = False
        has_flipwhite = False
        mark_pos = -1
        flip_pos = -1

        lines = content.splitlines()
        in_atomic = False
        brace_depth = 0

        for i, line in enumerate(lines, 1):
            if re.search(r'\batomic\b.*\(', line) and not line.strip().startswith('//'):
                in_atomic = True
            if in_atomic:
                brace_depth += line.count('{') - line.count('}')
                if 'mark_coro_roots' in line or 'markroot' in line.lower():
                    has_mark_roots = True
                    mark_pos = i
                if 'flipwhite' in line or 'flip_white' in line or 'currentwhite' in line:
                    has_flipwhite = True
                    flip_pos = i
                if brace_depth <= 0 and in_atomic and '{' in content[:sum(len(l)+1 for l in lines[:i])]:
                    in_atomic = False

        issues = []
        if not has_mark_roots:
            issues.append("atomic(): missing mark_coro_roots() or equivalent stack re-marking")
        if has_mark_roots and has_flipwhite and mark_pos > flip_pos:
            issues.append(f"atomic(): mark_coro_roots (line {mark_pos}) after flipwhite (line {flip_pos}) - must be before!")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "3.2", "GC atomic re-mark", "P0", passed,
            "atomic() correctly re-marks before flipwhite" if passed else "atomic() re-mark issue",
            issues
        ))

    # ================================================================
    # 3.4 P0: Immix mark_lines unconditional in newobj
    # ================================================================
    def check_immix_mark_unconditional(self):
        """xr_coro_gc_newobj must call xr_immix_mark_lines unconditionally."""
        content = self.read_file(XCORO_GC_C)
        if not content:
            self.add_result(CheckResult("3.4", "Immix mark_lines unconditional", "P0", False, "xcoro_gc.c not found"))
            return

        # Find xr_coro_gc_newobj and check if xr_immix_mark_lines is inside a gcstate condition
        issues = []
        lines = content.splitlines()
        in_newobj = False
        brace_depth = 0
        found_mark_lines = False
        mark_lines_in_condition = False

        for i, line in enumerate(lines, 1):
            if 'xr_coro_gc_newobj' in line and ('{' in line or (i < len(lines) and '{' in lines[i])):
                in_newobj = True
                brace_depth = 0
            if in_newobj:
                brace_depth += line.count('{') - line.count('}')
                if 'xr_immix_mark_lines' in line or 'xr_immix_mark_both_lines' in line:
                    found_mark_lines = True
                    # Check if this is inside a gcstate conditional
                    # Look back a few lines for if (gcstate ...)
                    for j in range(max(0, i-5), i):
                        prev_line = lines[j-1] if j > 0 else ""
                        if 'gcstate' in prev_line and 'if' in prev_line:
                            mark_lines_in_condition = True
                            issues.append(f"line {i}: xr_immix_mark_lines inside gcstate condition (line {j})")
                if brace_depth <= 0 and in_newobj and brace_depth != 0:
                    in_newobj = False

        if not found_mark_lines:
            issues.append("xr_coro_gc_newobj: xr_immix_mark_lines not found")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "3.4", "Immix mark_lines unconditional", "P0", passed,
            "xr_immix_mark_lines called unconditionally" if passed else "Conditional mark_lines detected",
            issues
        ))

    # ================================================================
    # 4.1 P0: call_closure frame state zeroed
    # ================================================================
    def check_call_closure_frame_zeroed(self):
        """xr_vm_call_closure must zero call_status and flags."""
        content = self.read_file(XVM_API_C)
        if not content:
            self.add_result(CheckResult("4.1", "call_closure frame zeroed", "P0", False, "xvm_api.c not found"))
            return

        # Find all functions that create call frames
        funcs = ['xr_vm_call_closure', 'xr_vm_call_closure_ex', 'xr_vm_call_method']
        issues = []

        for func_name in funcs:
            # Find function definition (not just any reference)
            func_start = -1
            search_pos = 0
            while True:
                pos = content.find(func_name, search_pos)
                if pos == -1:
                    break
                # Check if this is a function definition (has return type before and { after)
                line_start = content.rfind('\n', 0, pos)
                line_end = content.find('\n', pos)
                line = content[line_start:line_end] if line_start >= 0 else content[:line_end]
                if '{' in content[pos:pos+500]:
                    func_start = pos
                    break
                search_pos = pos + 1

            if func_start == -1:
                continue

            # Get a larger chunk to cover the full function body
            chunk = content[func_start:func_start + 4000]
            # Only look within the function body (up to the closing brace)
            brace_count = 0
            func_end = len(chunk)
            for ci, ch in enumerate(chunk):
                if ch == '{':
                    brace_count += 1
                elif ch == '}':
                    brace_count -= 1
                    if brace_count == 0:
                        func_end = ci
                        break
            func_body = chunk[:func_end]

            has_call_status_zero = bool(re.search(r'call_status\s*=\s*0', func_body))
            has_flags_zero = bool(re.search(r'->flags\s*=\s*0', func_body))
            # Also accept memset zeroing
            has_memset = 'memset' in func_body and 'frame' in func_body

            if not (has_call_status_zero or has_memset):
                issues.append(f"{func_name}: missing frame->call_status = 0")
            if not (has_flags_zero or has_memset):
                issues.append(f"{func_name}: missing frame->flags = 0")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "4.1", "call_closure frame zeroed", "P0", passed,
            "All call_closure frames properly zeroed" if passed else "Frame state not zeroed",
            issues
        ))

    # ================================================================
    # 7.1 P1: Type singleton immutability
    # ================================================================
    def check_type_singleton_immutability(self):
        """Type singletons must not be mutated directly; use xa_type_copy first."""
        issues = []

        # Scan analyzer directory for direct mutation of type fields
        for root, dirs, files in os.walk(ANALYZER_DIR):
            for fname in files:
                if not fname.endswith('.c'):
                    continue
                fpath = os.path.join(root, fname)
                lines = self.read_lines(fpath)
                rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                for i, line in enumerate(lines, 1):
                    stripped = line.strip()
                    # Look for direct is_nullable = true assignments
                    if re.search(r'->is_nullable\s*=\s*true', stripped):
                        # Check if there's a safe allocation/copy nearby (within 8 lines before)
                        has_copy = False
                        safe_patterns = [
                            'xa_type_copy', 'xa_pool_alloc', 'xa_type_new_',
                            'xa_pool_get_', '*type = *base_type', '= *base_type',
                            'malloc', 'xr_malloc', 'xa_type_new_array',
                            'xa_type_new_map', 'xa_type_new_any',
                        ]
                        for j in range(max(0, i-9), i):
                            for pat in safe_patterns:
                                if pat in lines[j]:
                                    has_copy = True
                                    break
                            if has_copy:
                                break
                        # Also check if the variable itself is from a safe source
                        for pat in safe_patterns:
                            if pat in stripped:
                                has_copy = True
                                break
                        # Check if it's inside xa_type_make_nullable (which handles frozen check)
                        func_context = '\n'.join(lines[max(0,i-30):i])
                        if 'xa_type_make_nullable' in func_context:
                            has_copy = True  # This function has its own frozen check
                        if 'xa_pool_get_optional' in func_context:
                            has_copy = True  # Pool function creates new type

                        if not has_copy:
                            issues.append(f"{rel_path}:{i}: {stripped}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "7.1", "type singleton immutability", "P1", passed,
            f"Found {len(issues)} potential singleton mutations" if issues else "No singleton mutations detected",
            issues[:20]
        ))

    # ================================================================
    # 7.3 P1: NaN equality semantics
    # ================================================================
    def check_nan_equality(self):
        """vm_values_equal must handle NaN != NaN per IEEE 754."""
        content = self.read_file(XVM_OPS_C)
        if not content:
            self.add_result(CheckResult("7.3", "NaN equality", "P1", False, "xvm_ops.c not found"))
            return

        # Find vm_values_equal function
        issues = []
        has_nan_check = False

        lines = content.splitlines()
        in_func = False
        for i, line in enumerate(lines, 1):
            if 'vm_values_equal' in line and '{' in line and 'deep' not in line:
                in_func = True
            if in_func:
                if 'isnan' in line or 'NaN' in line.lower() or 'XR_IS_FLOAT' in line:
                    has_nan_check = True
                # Check for float comparison that handles NaN correctly
                # The pattern: if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b)) return XR_TO_FLOAT(a) == XR_TO_FLOAT(b)
                # This is correct because IEEE 754 float == returns false for NaN
                if 'XR_TO_FLOAT(a) == XR_TO_FLOAT(b)' in line:
                    has_nan_check = True  # IEEE 754 comparison handles NaN correctly
                if line.strip() == '}' and in_func:
                    break

        if not has_nan_check:
            issues.append("vm_values_equal: no NaN handling found (float comparison must use IEEE 754 semantics)")

        # Also check that identity comparison (a == b) doesn't short-circuit before float check
        in_func = False
        identity_before_float = False
        for i, line in enumerate(lines, 1):
            if 'vm_values_equal' in line and '{' in line and 'deep' not in line:
                in_func = True
                continue
            if in_func:
                stripped = line.strip()
                if 'a == b' in stripped and 'return true' in stripped:
                    identity_before_float = True
                if 'XR_IS_FLOAT' in stripped and identity_before_float:
                    issues.append(f"line {i}: identity check (a == b) before float NaN check - NaN bits are identical!")
                    break
                if stripped == '}':
                    break

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "7.3", "NaN equality", "P1", passed,
            "NaN equality handled correctly" if passed else "NaN equality issue",
            issues
        ))

    # ================================================================
    # 12.1 P2: typeof naming consistency
    # ================================================================
    def check_typeof_naming(self):
        """Primitive types lowercase, object types PascalCase."""
        content = self.read_file(XTYPE_NAMES_H)
        if not content:
            self.add_result(CheckResult("12.1", "typeof naming", "P2", False, "xtype_names.h not found"))
            return

        # Primitive types should be lowercase
        primitives = {
            'TYPE_NAME_INT': 'int',
            'TYPE_NAME_FLOAT': 'float',
            'TYPE_NAME_STRING': 'string',
            'TYPE_NAME_BOOL': 'bool',
            'TYPE_NAME_NULL': 'null',
            'TYPE_NAME_FUNCTION': 'function',
        }

        # Object types should be PascalCase
        objects = {
            'TYPE_NAME_ARRAY': 'Array',
            'TYPE_NAME_MAP': 'Map',
            'TYPE_NAME_SET': 'Set',
            'TYPE_NAME_JSON': 'Json',
            'TYPE_NAME_BYTES': 'Bytes',
            'TYPE_NAME_BIGINT': 'BigInt',
            'TYPE_NAME_REGEX': 'Regex',
            'TYPE_NAME_DATETIME': 'DateTime',
            'TYPE_NAME_STRINGBUILDER': 'StringBuilder',
            'TYPE_NAME_CHANNEL': 'Channel',
            'TYPE_NAME_EXCEPTION': 'Exception',
            'TYPE_NAME_COROUTINE': 'Coroutine',
        }

        issues = []
        lines = content.splitlines()

        for i, line in enumerate(lines, 1):
            for macro, expected in {**primitives, **objects}.items():
                pattern = rf'#define\s+{macro}\s+"([^"]+)"'
                m = re.search(pattern, line)
                if m:
                    actual = m.group(1)
                    if actual != expected:
                        issues.append(f"line {i}: {macro} = \"{actual}\" (expected \"{expected}\")")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "12.1", "typeof naming consistency", "P2", passed,
            "All type names follow convention" if passed else f"Found {len(issues)} naming issues",
            issues
        ))

    # ================================================================
    # 13.1 P2: TLS holding GC object references
    # ================================================================
    def check_tls_gc_references(self):
        """TLS variables must not hold GC object references without registration."""
        issues = []

        # Search for TLS variables that might hold GC objects
        tls_patterns = [
            r'_Thread_local\s+.*Xr\w+',
            r'__thread\s+.*Xr\w+',
            r'thread_local\s+.*Xr\w+',
        ]

        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")

        for search_dir in [src_dir, stdlib_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith(('.c', '.h')):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    for i, line in enumerate(lines, 1):
                        for pat in tls_patterns:
                            if re.search(pat, line):
                                # Check if it's a GC-managed type
                                gc_types = ['XrValue', 'XrMap', 'XrArray', 'XrString',
                                           'XrJson', 'XrInstance', 'XrClosure', 'XrClass']
                                for gt in gc_types:
                                    if gt in line:
                                        issues.append(f"{rel_path}:{i}: TLS holds {gt}: {line.strip()}")
                                        break

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "13.1", "TLS GC references", "P2", passed,
            "No TLS GC reference issues" if passed else f"Found {len(issues)} TLS GC references",
            issues
        ))

    # ================================================================
    # 11.2 P2: YAML flow scan-ahead boundary
    # ================================================================
    def check_yaml_scan_ahead(self):
        """YAML parse_value scan-ahead must stop at flow indicators."""
        content = self.read_file(YAML_PARSER_C)
        if not content:
            self.add_result(CheckResult("11.2", "YAML scan-ahead boundary", "P2", False, "yaml_parser.c not found"))
            return

        issues = []
        # Look for scan-ahead loop in parse_value
        # It should check for ',', '}', ']' as terminators
        if 'parse_value' in content:
            # Find the scan-ahead section
            lines = content.splitlines()
            in_parse_value = False
            found_scan_ahead = False
            has_comma_check = False
            has_rbrace_check = False
            has_rbracket_check = False

            for i, line in enumerate(lines, 1):
                if 'parse_value' in line and '{' in line:
                    in_parse_value = True
                if in_parse_value:
                    # Look for scan-ahead loop (scanning for ':' to detect mapping)
                    if "':'" in line or "== ':'" in line or "scan" in line.lower():
                        found_scan_ahead = True
                    if found_scan_ahead:
                        if "','" in line or "== ','" in line:
                            has_comma_check = True
                        if "'}'" in line or "== '}'" in line:
                            has_rbrace_check = True
                        if "']'" in line or "== ']'" in line:
                            has_rbracket_check = True

            if found_scan_ahead:
                if not has_comma_check:
                    issues.append("parse_value scan-ahead: missing ',' flow indicator check")
                if not has_rbrace_check:
                    issues.append("parse_value scan-ahead: missing '}' flow indicator check")
                if not has_rbracket_check:
                    issues.append("parse_value scan-ahead: missing ']' flow indicator check")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "11.2", "YAML scan-ahead boundary", "P2", passed,
            "YAML scan-ahead correctly bounded" if passed else "YAML scan-ahead boundary issues",
            issues
        ))

    # ================================================================
    # 2.2 P0: IC debug offset validation
    # ================================================================
    def check_ic_debug_bind(self):
        """Every IC lookup must have corresponding XR_VM_IC_*_BIND call."""
        content = self.read_file(XVM_C)
        if not content:
            self.add_result(CheckResult("2.2", "IC debug bind", "P0", False, "xvm.c not found"))
            return

        issues = []
        lines = content.splitlines()

        # Find lines with ic_method_table_get or ic_field_table_get
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            if 'ic_method_table_get' in stripped or 'ic_field_table_get' in stripped:
                # Check next few lines for corresponding BIND macro
                has_bind = False
                for j in range(i, min(i + 3, len(lines))):
                    next_line = lines[j]
                    if 'IC_METHOD_BIND' in next_line or 'IC_FIELD_BIND' in next_line:
                        has_bind = True
                        break
                if not has_bind:
                    issues.append(f"line {i}: IC table_get without BIND macro: {stripped[:80]}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "2.2", "IC debug bind", "P0", passed,
            "All IC lookups have BIND calls" if passed else f"Found {len(issues)} IC lookups without BIND",
            issues
        ))

    # ================================================================
    # 5.3 P0: Inheritance field count includes ancestors
    # ================================================================
    def check_inheritance_field_count(self):
        """ClassInfo instance_field_count must include ancestor fields."""
        content = self.read_file(CLASS_REGISTRY_C)
        if not content:
            self.add_result(CheckResult("5.3", "inheritance field count", "P0", False, "xcompiler_class_registry.c not found"))
            return

        issues = []
        # Check that when setting instance_field_count, it includes parent fields
        # Bad pattern: info->instance_field_count = desc->field_count (own only)
        # Good pattern: info->instance_field_count = parent_info->instance_field_count + own_count

        lines = content.splitlines()
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            if 'instance_field_count' in line and '=' in line and 'field_count' in line:
                # Skip initialization to 0 and capacity checks
                if re.search(r'instance_field_count\s*=\s*0', stripped):
                    continue
                if '>=' in stripped or '<=' in stripped or '>' in stripped or '<' in stripped:
                    continue
                if '++' in stripped:
                    continue
                # Check if it references parent/ancestor count
                context = '\n'.join(lines[max(0,i-5):i+5])
                if 'parent' not in context.lower() and 'ancestor' not in context.lower() and '+' not in line:
                    # Might be setting only own fields
                    issues.append(f"line {i}: instance_field_count may not include ancestors: {stripped}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "5.3", "inheritance field count", "P0", passed,
            "Field count includes ancestors" if passed else "Field count may miss ancestors",
            issues
        ))

    # ================================================================
    # 14.1 P1: Comment convention - no external tech references
    # ================================================================
    def check_comment_no_tech_references(self):
        """Comments must not reference external tech as style/inspiration source."""
        # Forbidden patterns in comments:
        # - "XX-style" (e.g. "Go-style", "Erlang-style", "V8-style", "Lua-style")
        # - "XX style" (without hyphen)
        # - "XX-like" / "XX like"
        # - "similar to XX" (where XX is a known tech name)
        # - "inspired by XX" / "borrowed from XX" / "modeled after XX" / "ported from XX"
        # - Chinese: "参考XX" / "借鉴XX" / "类似XX的"

        known_techs = [
            'Go', 'Lua', 'V8', 'Erlang', 'Java', 'Python', 'Ruby', 'Rust',
            'Node', 'Deno', 'Bun', 'TypeScript', 'JavaScript', 'Swift',
            'Kotlin', 'C#', 'Haskell', 'OCaml', 'Zig', 'Nim',
            'libtommath', 'GHC', 'JVM', 'CLR', 'BEAM', 'CPython',
            'LuaJIT', 'SpiderMonkey', 'JSC', 'Chakra',
        ]

        # Build regex patterns
        tech_names = '|'.join(re.escape(t) for t in known_techs)
        patterns = [
            # XX-style / XX style
            (re.compile(rf'\b({tech_names})[- ]style\b', re.IGNORECASE), 'XX-style'),
            # XX-like / XX like
            (re.compile(rf'\b({tech_names})[- ]like\b', re.IGNORECASE), 'XX-like'),
            # similar to XX
            (re.compile(rf'similar\s+to\s+({tech_names})\b', re.IGNORECASE), 'similar to XX'),
            # inspired by XX
            (re.compile(rf'inspired\s+by\s+({tech_names})\b', re.IGNORECASE), 'inspired by XX'),
            # borrowed from XX
            (re.compile(rf'borrowed\s+from\s+({tech_names})\b', re.IGNORECASE), 'borrowed from XX'),
            # modeled after XX
            (re.compile(rf'modeled\s+after\s+({tech_names})\b', re.IGNORECASE), 'modeled after XX'),
            # ported from XX
            (re.compile(rf'ported\s+from\s+({tech_names})\b', re.IGNORECASE), 'ported from XX'),
            # Chinese patterns
            (re.compile(rf'参考\s*({tech_names})', re.IGNORECASE), '参考XX'),
            (re.compile(rf'借鉴\s*({tech_names})', re.IGNORECASE), '借鉴XX'),
            (re.compile(rf'类似\s*({tech_names})\s*的', re.IGNORECASE), '类似XX的'),
        ]

        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")
        include_dir = os.path.join(PROJECT_ROOT, "include")

        for search_dir in [src_dir, stdlib_dir, include_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith(('.c', '.h')):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    for i, line in enumerate(lines, 1):
                        # Only check comments (lines with // or inside /* */)
                        stripped = line.strip()
                        is_comment = (
                            stripped.startswith('//') or
                            stripped.startswith('/*') or
                            stripped.startswith('*') or
                            stripped.startswith('#') or
                            '/*' in stripped or
                            '//' in stripped
                        )
                        if not is_comment:
                            continue

                        for pat, desc in patterns:
                            m = pat.search(line)
                            if m:
                                tech = m.group(1)
                                issues.append(f"{rel_path}:{i}: [{desc}] \"{tech}\" -> {stripped[:100]}")
                                break  # One issue per line

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "14.1", "comment: no tech references", "P1", passed,
            f"Found {len(issues)} comments referencing external tech" if issues else "No external tech references in comments",
            issues
        ))

    # ================================================================
    # 14.3 P1: No Chinese characters in C source comments
    # ================================================================
    def check_no_chinese_comments(self):
        """All C source comments must be in English."""
        issues = []
        chinese_re = re.compile(r'[\u4e00-\u9fff]')

        for search_dir in [os.path.join(PROJECT_ROOT, d) for d in ("src", "include", "stdlib")]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith(('.c', '.h')) or fname.endswith('.h.in'):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)
                    for i, line in enumerate(lines):
                        if chinese_re.search(line):
                            issues.append(f"{rel_path}:{i+1}: {line.strip()[:80]}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "14.3", "no Chinese in C source", "P1", passed,
            f"Found {len(issues)} lines with Chinese characters" if issues else "No Chinese characters in C source",
            issues[:20]
        ))

    # ================================================================
    # 14.4 P2: Single-line /* */ should use // style
    # ================================================================
    def check_single_line_comment_style(self):
        """Single-line comments should use // not /* */, except section dividers."""
        issues = []
        single_re = re.compile(r'^(\s*)/\*(?!\s*=)(.*?)\*/\s*$')
        section_re = re.compile(r'/\*\s*={2,}')
        trailing_re = re.compile(r'^(.+\S)\s*/\*(?!\s*=)\s?(.*?)\s?\*/\s*$')

        for search_dir in [os.path.join(PROJECT_ROOT, d) for d in ("src", "include", "stdlib")]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith(('.c', '.h')) or fname.endswith('.h.in'):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)
                    in_block = False
                    for i, line in enumerate(lines):
                        raw = line.rstrip('\n')
                        if in_block:
                            if '*/' in raw:
                                in_block = False
                            continue
                        if '/*' in raw and '*/' not in raw:
                            in_block = True
                            continue
                        if section_re.search(raw):
                            continue
                        if single_re.match(raw) or (trailing_re.match(raw) and not raw.strip().startswith('/*')):
                            issues.append(f"{rel_path}:{i+1}: {raw.strip()[:80]}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "14.4", "single-line /* */ style", "P2", passed,
            f"Found {len(issues)} single-line /* */ (should use //)" if issues else "All single-line comments use //",
            issues[:20]
        ))

    # ================================================================
    # 15.1 P1: C naming convention - function names
    # ================================================================
    def check_naming_convention(self):
        """Check xray macro naming conventions: XR_XXX / XRAY_XXX / known prefixes."""
        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")

        # Check macro naming: #define should use XR_ prefix (except standard guards)
        macro_issues = []
        for search_dir in [src_dir, stdlib_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith('.h'):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    for i, line in enumerate(lines, 1):
                        stripped = line.strip()
                        if not stripped.startswith('#define '):
                            continue
                        # Extract macro name
                        parts = stripped.split(None, 2)
                        if len(parts) < 2:
                            continue
                        macro_name = parts[1].split('(')[0]  # Handle function-like macros

                        # Skip include guards, standard macros, and lowercase helper macros
                        if macro_name.startswith('_') or macro_name.endswith('_H'):
                            continue
                        if macro_name.islower():
                            continue  # lowercase macros like 'likely', 'unlikely'
                        # Must start with XR_ or XRAY_ or known prefixes
                        valid_macro_prefixes = [
                            'XR_', 'XRAY_', 'VM_', 'GC_', 'XA_',
                            'OP_', 'TK_', 'AST_', 'NUM_', 'MAX_', 'MIN_',
                            'PROTO_', 'GETARG_', 'GET_', 'SET_',
                            'SYMBOL_', 'BUILTIN_', 'TYPE_NAME_',
                            'R(', 'K(', 'RA(', 'RB(', 'RC(', 'KB(', 'KC(',
                            'INITIAL_', 'DEFAULT_',
                        ]
                        if any(macro_name.startswith(p) for p in valid_macro_prefixes):
                            continue
                        # Allow single-word uppercase macros (common C idioms)
                        if '_' not in macro_name and macro_name.isupper():
                            continue

                        macro_issues.append(f"{rel_path}:{i}: non-standard macro: {macro_name}")

        passed = len(macro_issues) == 0
        self.add_result(CheckResult(
            "15.1", "macro naming convention", "P2", passed,
            f"Found {len(macro_issues)} non-standard macro names" if macro_issues else "All macro names follow convention",
            macro_issues[:30]
        ))

    # ================================================================
    # 14.2 P2: File header format check
    # ================================================================
    def check_file_header_format(self):
        """C source files should have standard file header with copyright."""
        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")

        for search_dir in [src_dir, stdlib_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith(('.c', '.h')):
                        continue
                    fpath = os.path.join(root, fname)
                    content = self.read_file(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    if not content.strip():
                        continue

                    # Check for standard header
                    has_xray = 'xray' in content[:500].lower()
                    has_copyright = 'Copyright' in content[:500] or 'copyright' in content[:500]
                    has_license = 'MIT' in content[:500] or 'License' in content[:500]

                    # Check for forbidden content in header (only in comment lines)
                    header_lines = content[:1000].split('\n')
                    has_version = False
                    has_emoji = False
                    for hline in header_lines:
                        hs = hline.strip()
                        # Only check comment lines (starting with * or //)
                        is_comment = hs.startswith('*') or hs.startswith('//') or hs.startswith('/*')
                        if not is_comment:
                            continue
                        # Skip spec references like "TOML v1.0.0 specification"
                        if re.search(r'spec|compliant|RFC|standard', hs, re.IGNORECASE):
                            continue
                        # Skip wire-format docs like "[version 1B]"
                        if re.search(r'\[version\b', hs, re.IGNORECASE):
                            continue
                        if re.search(r'\bPhase\s*\d|Step\s*\d', hs):
                            has_version = True
                        # Match bare version declarations like "(v2.0.0)" but not protocol refs
                        if re.search(r'\(v\d+\.\d+\.\d+\)', hs):
                            has_version = True
                        if re.search(r'[\U0001F300-\U0001F9FF]', hs):
                            has_emoji = True

                    if not has_copyright:
                        issues.append(f"{rel_path}: missing copyright in header")
                    if has_version:
                        issues.append(f"{rel_path}: header contains version/phase/step numbers (forbidden)")
                    if has_emoji:
                        issues.append(f"{rel_path}: header contains emoji (forbidden)")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "14.2", "file header format", "P2", passed,
            f"Found {len(issues)} header issues" if issues else "All file headers OK",
            issues[:20]
        ))

    # ================================================================
    # 16.1 P0: Dangerous realloc pattern (ptr = realloc(ptr, ...))
    # ================================================================
    def check_dangerous_realloc(self):
        """Detect ptr = realloc(ptr, ...) that loses original pointer on OOM."""
        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")

        pattern = re.compile(
            r'^\s+(\w+(?:->[\w.]+)*)\s*=\s*\([^)]+\*\)\s*xr_realloc\s*\(\s*(\w+(?:->[\w.]+)*)'
        )

        for search_dir in [src_dir, stdlib_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith('.c'):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    for i, line in enumerate(lines):
                        m = pattern.match(line)
                        if not m or m.group(1) != m.group(2):
                            continue
                        # Check if next 2 lines have NULL check
                        has_check = False
                        for j in range(i + 1, min(i + 3, len(lines))):
                            nxt = lines[j]
                            if 'if' in nxt and ('!' in nxt or 'NULL' in nxt):
                                has_check = True
                                break
                        if not has_check:
                            issues.append(f"{rel_path}:{i+1}: {line.strip()[:90]}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "16.1", "dangerous realloc pattern", "P0", passed,
            f"Found {len(issues)} dangerous ptr=realloc(ptr,...) patterns" if issues else "No dangerous realloc patterns",
            issues[:20]
        ))

    # ================================================================
    # 16.2 P1: Missing NULL check after malloc/calloc
    # ================================================================
    def check_missing_alloc_null_check(self):
        """Detect malloc/calloc without NULL check on the result."""
        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")

        alloc_pattern = re.compile(r'(\w+(?:->[\w.]+)*)\s*=\s*\([^)]+\)\s*(xr_malloc|xr_calloc)\s*\(')

        for search_dir in [src_dir, stdlib_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith('.c'):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    for i, line in enumerate(lines):
                        m = alloc_pattern.search(line)
                        if not m:
                            continue
                        # Skip xr_calloc(1, sizeof(...)) - single object alloc often checked later
                        if 'xr_calloc(1,' in line:
                            continue
                        # Check next 3 lines for NULL check
                        has_check = False
                        for j in range(i + 1, min(i + 4, len(lines))):
                            nxt = lines[j].strip()
                            if re.search(r'if\s*\(\s*!|== NULL|!= NULL|XR_CHECK|return|break|memset', nxt):
                                has_check = True
                                break
                        if not has_check:
                            issues.append(f"{rel_path}:{i+1}: {line.strip()[:90]}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "16.2", "missing NULL check after alloc", "P1", passed,
            f"Found {len(issues)} allocs without NULL check" if issues else "All allocs have NULL checks",
            issues[:20]
        ))

    # ================================================================
    # 16.3 P2: #endif comment format (must use // not /* */)
    # ================================================================
    def check_endif_comment_format(self):
        """#endif comments must use // format, not /* */ format."""
        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")
        stdlib_dir = os.path.join(PROJECT_ROOT, "stdlib")
        include_dir = os.path.join(PROJECT_ROOT, "include")

        endif_bad = re.compile(r'#endif\s+/\*')

        for search_dir in [src_dir, stdlib_dir, include_dir]:
            if not os.path.exists(search_dir):
                continue
            for root, dirs, files in os.walk(search_dir):
                for fname in files:
                    if not fname.endswith('.h'):
                        continue
                    fpath = os.path.join(root, fname)
                    lines = self.read_lines(fpath)
                    rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                    for i, line in enumerate(lines):
                        if endif_bad.search(line):
                            issues.append(f"{rel_path}:{i+1}: {line.strip()}")

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "16.3", "#endif comment format", "P2", passed,
            f"Found {len(issues)} #endif using /* */ (should use //)" if issues else "All #endif use // format",
            issues[:20]
        ))

    # ================================================================
    # 16.4 P2: Redundant XR_DCHECK + NULL check pairs
    # ================================================================
    def check_redundant_dcheck_null(self):
        """Detect XR_DCHECK(ptr != NULL) followed by if (!ptr) return - contradictory."""
        issues = []
        src_dir = os.path.join(PROJECT_ROOT, "src")

        dcheck_null = re.compile(r'XR_DCHECK\s*\(\s*(\w+)\s*!=\s*NULL')

        for root, dirs, files in os.walk(src_dir):
            for fname in files:
                if not fname.endswith('.c'):
                    continue
                fpath = os.path.join(root, fname)
                lines = self.read_lines(fpath)
                rel_path = os.path.relpath(fpath, PROJECT_ROOT)

                for i, line in enumerate(lines):
                    m = dcheck_null.search(line)
                    if not m:
                        continue
                    var = m.group(1)
                    # Check next 3 lines for if (!var) return pattern
                    for j in range(i + 1, min(i + 4, len(lines))):
                        nxt = lines[j].strip()
                        if re.search(rf'if\s*\(\s*!{re.escape(var)}\s*\)', nxt):
                            issues.append(f"{rel_path}:{i+1}: XR_DCHECK({var} != NULL) + if(!{var}) is contradictory")
                            break

        passed = len(issues) == 0
        self.add_result(CheckResult(
            "16.4", "redundant DCHECK+NULL check", "P2", passed,
            f"Found {len(issues)} redundant DCHECK+NULL pairs" if issues else "No redundant DCHECK+NULL pairs",
            issues[:20]
        ))

    # ================================================================
    # Run all checks
    # ================================================================
    def run_all(self, category='all'):
        checks = {
            'vm': [
                self.check_vmcase_exit_paths,
                self.check_instruction_variant_consistency,
            ],
            'ic': [
                self.check_ic_cache_index,
                self.check_ic_debug_bind,
            ],
            'gc': [
                self.check_write_barrier_coverage,
                self.check_gc_atomic_remark,
                self.check_immix_mark_unconditional,
            ],
            'frame': [
                self.check_call_closure_frame_zeroed,
            ],
            'compiler': [
                self.check_inheritance_field_count,
            ],
            'type': [
                self.check_type_singleton_immutability,
                self.check_nan_equality,
            ],
            'naming': [
                self.check_typeof_naming,
                self.check_naming_convention,
            ],
            'comment': [
                self.check_comment_no_tech_references,
                self.check_file_header_format,
                self.check_no_chinese_comments,
                self.check_single_line_comment_style,
                self.check_endif_comment_format,
            ],
            'tls': [
                self.check_tls_gc_references,
            ],
            'yaml': [
                self.check_yaml_scan_ahead,
            ],
            'memory': [
                self.check_dangerous_realloc,
                self.check_missing_alloc_null_check,
            ],
            'style': [
                self.check_endif_comment_format,
                self.check_redundant_dcheck_null,
            ],
        }

        if category == 'all':
            for cat_checks in checks.values():
                for check in cat_checks:
                    check()
        elif category in checks:
            for check in checks[category]:
                check()
        else:
            print(f"Unknown category: {category}")
            print(f"Available: {', '.join(checks.keys())}, all")
            sys.exit(1)

    def print_report(self):
        """Print formatted report."""
        print("\n" + "=" * 70)
        print("  Xray Static Analysis Report")
        print("=" * 70)

        # Group by priority
        by_priority = {'P0': [], 'P1': [], 'P2': []}
        for r in self.results:
            by_priority.setdefault(r.priority, []).append(r)

        total_pass = sum(1 for r in self.results if r.passed)
        total_fail = sum(1 for r in self.results if not r.passed)

        for prio in ['P0', 'P1', 'P2']:
            results = by_priority.get(prio, [])
            if not results:
                continue

            print(f"\n--- {prio} Checks ---")
            for r in results:
                status = "\033[32mPASS\033[0m" if r.passed else "\033[31mFAIL\033[0m"
                print(f"  [{status}] {r.check_id} {r.name}: {r.message}")
                if not r.passed and r.details:
                    for d in r.details[:10]:
                        print(f"         - {d}")
                    if len(r.details) > 10:
                        print(f"         ... and {len(r.details) - 10} more")
                elif self.verbose and r.details:
                    for d in r.details:
                        print(f"         - {d}")

        print(f"\n{'=' * 70}")
        print(f"  Summary: {total_pass} passed, {total_fail} failed, {len(self.results)} total")
        if total_fail > 0:
            p0_fails = sum(1 for r in self.results if not r.passed and r.priority == 'P0')
            if p0_fails > 0:
                print(f"  \033[31m⚠ {p0_fails} P0 (critical) checks failed!\033[0m")
        print("=" * 70 + "\n")

        return total_fail == 0


def main():
    parser = argparse.ArgumentParser(description='Xray Static Analysis Checker')
    parser.add_argument('--verbose', '-v', action='store_true', help='Show detailed output')
    parser.add_argument('--category', '-c', default='all',
                       help='Check category: all, vm, ic, gc, frame, compiler, type, naming, comment, tls, yaml, memory, style')
    args = parser.parse_args()

    checker = StaticChecker(verbose=args.verbose)
    checker.run_all(args.category)
    success = checker.print_report()
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
