#!/usr/bin/env python3
"""Migrate regression tests from top-level assertions to @test functions.

Strategy:
- Keep top-level declarations (struct, class, enum, fn, interface, import) at top level
- Wrap remaining executable code in a single @test function
- Special case: fn main() + main() call → convert to @test fn
- Remove trailing print("ok")/print("passed") etc.
- Skip module files and DAP fixtures
"""

import os
import re
import sys

SKIP_FILES = {
    # DAP fixtures: not tests
    "tests/regression/dap/fixtures/breakpoint.xr",
    "tests/regression/dap/fixtures/exception.xr",
    "tests/regression/dap/fixtures/funcbp.xr",
    "tests/regression/dap/fixtures/hello.xr",
    "tests/regression/dap/fixtures/logpoint.xr",
    # Module helper files: imported by other tests
    "tests/regression/10_stdlib/modules/mod_a.xr",
    "tests/regression/10_stdlib/modules/mod_b.xr",
    "tests/regression/10_stdlib/reexport_test/index.xr",
    "tests/regression/10_stdlib/reexport_test/product.xr",
    "tests/regression/10_stdlib/reexport_test/user.xr",
}

# Patterns for trailing prints to remove
TRAILING_PRINT_RE = re.compile(
    r'^print\s*\(\s*"[^"]*(?:pass|ok|done|通过|成功|All\s+\w+\s+tests)[^"]*"\s*\)\s*$',
    re.IGNORECASE
)

# Decorative prints (separator lines)
DECORATIVE_PRINT_RE = re.compile(
    r'^print\s*\(\s*"[=\-*+]{5,}"\s*\)\s*$'
)
DECORATIVE_PRINT2_RE = re.compile(
    r'^print\s*\(\s*"[=\-*+]"\s*\*\s*\d+\s*\)\s*$'
)


def is_declaration_line(line):
    """Check if a line starts a top-level declaration."""
    stripped = line.lstrip()
    # struct, class, enum, interface declarations
    if re.match(r'^(struct|class|enum|interface)\s+\w+', stripped):
        return True
    # fn declarations (but not fn calls)
    if re.match(r'^(fn|async\s+fn)\s+\w+\s*[\(<]', stripped):
        return True
    # import statements
    if re.match(r'^import\s+', stripped):
        return True
    # export declarations
    if re.match(r'^export\s+', stripped):
        return True
    return False


def is_blank_or_comment(line):
    """Check if line is blank or a comment."""
    stripped = line.strip()
    return stripped == '' or stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('**') or stripped.startswith('*/')


def find_block_end(lines, start):
    """Find the end of a braced block starting at 'start'.
    Returns the index of the closing brace line."""
    depth = 0
    for i in range(start, len(lines)):
        depth += lines[i].count('{') - lines[i].count('}')
        if depth <= 0:
            return i
    return len(lines) - 1


def is_trailing_print(line):
    """Check if line is a trailing 'passed' print to remove."""
    stripped = line.strip()
    if TRAILING_PRINT_RE.match(stripped):
        return True
    if DECORATIVE_PRINT_RE.match(stripped):
        return True
    if DECORATIVE_PRINT2_RE.match(stripped):
        return True
    return False


def has_main_pattern(lines):
    """Check if file has fn main() {...} followed by main() call."""
    has_main_fn = False
    has_main_call = False
    main_fn_line = -1
    main_call_line = -1
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r'^fn\s+main\s*\(\s*\)', stripped):
            has_main_fn = True
            main_fn_line = i
        if stripped == 'main()' and has_main_fn:
            has_main_call = True
            main_call_line = i
    return has_main_fn and has_main_call, main_fn_line, main_call_line


def migrate_main_pattern(lines, main_fn_line, main_call_line):
    """Convert fn main() {...} main() to @test fn test_main() {...}."""
    result = []
    for i, line in enumerate(lines):
        if i == main_fn_line:
            # Remove blank line before @test if present
            while result and result[-1].strip() == '':
                result.pop()
            result.append('')
            result.append('@test')
            new_line = re.sub(r'^fn\s+main\s*\(\s*\)\s*:?\s*\w*', 'fn test_main()', line)
            result.append(new_line)
        elif i == main_call_line:
            # Remove main() call
            continue
        else:
            result.append(line)
    return result


def migrate_file(filepath):
    """Migrate a single file to @test style. Returns (new_content, changed)."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Already has @test → skip
    if re.search(r'^\s*@test', content, re.MULTILINE):
        return content, False

    lines = content.split('\n')

    # Check for fn main() pattern
    has_main, main_fn_line, main_call_line = has_main_pattern(lines)
    if has_main:
        new_lines = migrate_main_pattern(lines, main_fn_line, main_call_line)
        return '\n'.join(new_lines) + '\n', True

    # General case: separate declarations from executable code
    # First pass: identify declaration blocks (and their extent with braces)
    decl_ranges = set()  # line indices that are part of declarations
    i = 0
    while i < len(lines):
        if is_declaration_line(lines[i]):
            start = i
            # Find the end of this declaration (including braced body)
            if '{' in lines[i]:
                end = find_block_end(lines, i)
            else:
                # Single-line declaration or forward decl
                end = i
            for j in range(start, end + 1):
                decl_ranges.add(j)
            i = end + 1
        else:
            i += 1

    # Collect header comments (before any code)
    header_end = 0
    for i, line in enumerate(lines):
        if is_blank_or_comment(line):
            header_end = i + 1
        else:
            break

    # Separate into: header, declarations, executable code
    header_lines = []
    decl_lines = []
    exec_lines = []

    # Header: initial comments
    for i in range(header_end):
        header_lines.append(lines[i])

    # Rest: declarations vs executable
    for i in range(header_end, len(lines)):
        if i in decl_ranges:
            decl_lines.append(lines[i])
        else:
            exec_lines.append(lines[i])

    # Remove trailing blank lines and 'print("passed")' from exec_lines
    while exec_lines and exec_lines[-1].strip() == '':
        exec_lines.pop()
    while exec_lines and is_trailing_print(exec_lines[-1]):
        exec_lines.pop()
    while exec_lines and exec_lines[-1].strip() == '':
        exec_lines.pop()

    # If no executable code, nothing to wrap
    if not any(line.strip() for line in exec_lines):
        return content, False

    # Build the test function name from filename
    basename = os.path.splitext(os.path.basename(filepath))[0]
    # Remove numeric prefix
    fn_name = re.sub(r'^\d+_?', '', basename)
    if not fn_name:
        fn_name = basename
    fn_name = 'test_' + fn_name

    # Build output
    result = []

    # Header comments
    for line in header_lines:
        result.append(line)

    # Declarations (with blank line separation)
    if decl_lines:
        # Add blank line after header if needed
        if result and result[-1].strip() != '':
            result.append('')
        for line in decl_lines:
            result.append(line)

    # @test function wrapping executable code
    if result and result[-1].strip() != '':
        result.append('')

    result.append('@test')
    result.append(f'fn {fn_name}() {{')

    for line in exec_lines:
        if line.strip() == '':
            result.append('')
        else:
            result.append('    ' + line)

    result.append('}')
    result.append('')

    return '\n'.join(result) + '\n', True


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    regression_dir = os.path.join(root, 'tests', 'regression')

    # Find files without @test
    migrate_count = 0
    skip_count = 0
    error_count = 0

    for dirpath, dirnames, filenames in os.walk(regression_dir):
        for fname in sorted(filenames):
            if not fname.endswith('.xr'):
                continue
            filepath = os.path.join(dirpath, fname)
            relpath = os.path.relpath(filepath, root)

            if relpath in SKIP_FILES:
                skip_count += 1
                continue

            try:
                new_content, changed = migrate_file(filepath)
                if changed:
                    with open(filepath, 'w') as f:
                        f.write(new_content)
                    migrate_count += 1
                    print(f"  MIGRATED: {relpath}")
            except Exception as e:
                error_count += 1
                print(f"  ERROR: {relpath}: {e}")

    print(f"\nDone: {migrate_count} migrated, {skip_count} skipped, {error_count} errors")


if __name__ == '__main__':
    main()
