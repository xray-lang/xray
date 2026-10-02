"""Generate the runtime bundle exact set from its actual CMake source owners."""
from __future__ import annotations
import argparse
import json
import re
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)
OWNERS = {
    'src/base/CMakeLists.txt': ['xray_compile_resources'],
    'src/xir/CMakeLists.txt': ['xray_xir_admission', 'xray_xir_declarations', 'xray_xir_scalar'],
    'src/execution/xir_runtime_host.cmake': ['xray_xir_runtime_host'],
}
GENERATED_RUNTIME_ROOTS = ['src/xir/xxir_equal.h', 'src/xir/xxir_panic.h',
                           'src/xir/xxir_output.h', 'src/xir/xxir_nullable.h', 'src/shared/xr_error_core.h']

def recipe(root: Path) -> dict:
    root = root.resolve()
    groups: dict[str, list[str]] = {}
    for filename, owners in OWNERS.items():
        path = root / filename
        text = path.read_text(encoding='utf-8-sig')
        for owner in owners:
            declaration = re.findall(r'add_library\(' + owner + r'\s+STATIC\s+([^\n)]*)\)', text)
            if len(declaration) != 1:
                raise ValueError(f'exactly one literal source owner required: {owner}')
            groups[owner] = [(path.parent / spelling).resolve().relative_to(root).as_posix()
                             for spelling in declaration[0].split()]
            if owner == 'xray_xir_runtime_host':
                windows = re.findall(r'if\(WIN32\)\s*target_sources\(' + owner +
                                     r'\s+PRIVATE\s+([^\n)]*)\)', text)
                if len(windows) != 1:
                    raise ValueError('exactly one Windows runtime host source branch required')
                groups[owner] += [(path.parent / spelling).resolve().relative_to(root).as_posix()
                                  for spelling in windows[0].split()]
    roots = [name for entries in groups.values() for name in entries] + GENERATED_RUNTIME_ROOTS
    pending = [root / name for name in roots]
    entries: dict[str, int] = {}
    while pending:
        path = pending.pop().resolve()
        relative = path.relative_to(root).as_posix()
        if relative in entries:
            continue
        if relative.startswith(('src/frontend/', 'src/vm/', 'src/program/', 'src/aot/',
                                'src/api/', 'src/runtime/', 'src/coro/')):
            raise ValueError(f'compiler or legacy runtime dependency: {relative}')
        kind = 3 if relative.endswith('.inc.c') else 1 if relative.endswith('.c') else \
            2 if relative.endswith('.h') else 4 if relative.endswith('.def') else 0
        if not kind or not path.is_file():
            raise ValueError(f'unsupported or missing runtime dependency: {relative}')
        entries[relative] = kind
        for spelling in INCLUDE.findall(path.read_text(encoding='utf-8-sig')):
            candidates = [path.parent / spelling, root / 'src' / spelling,
                          root / 'generated' / spelling, root / 'include' / spelling]
            found = next((candidate for candidate in candidates if candidate.is_file()), None)
            if found is None:
                raise ValueError(f'missing quoted dependency: {relative}: {spelling}')
            pending.append(found)
    for owner in groups:
        entries[f'lib/{owner}.lib'] = 5
    if len(entries) > 512:
        raise ValueError('runtime recipe exceeds its file cap')
    return {'archive_source_groups': groups,
            'files': [{'path': name, 'kind': entries[name]} for name in sorted(entries)]}

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--record')
    args = parser.parse_args()
    result = recipe(Path(args.root))
    lines = ['/* Generated from the actual runtime CMake source owners. */',
             'static const XrXirSdkRecipeFile sdk_recipe_files[] = {']
    lines.extend('    {' + json.dumps(entry['path']) + ', ' + str(entry['kind']) + 'u},'
                 for entry in result['files'])
    lines.append('};')
    Path(args.output).write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    if args.record:
        Path(args.record).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')

if __name__ == '__main__':
    main()
