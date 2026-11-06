"""Generate the runtime bundle exact set from its actual CMake source owners."""
from __future__ import annotations
import argparse
import json
import os
import re
from pathlib import Path

INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)
OWNERS = {
    'src/base/CMakeLists.txt': ['xray_compile_resources'],
    'src/xir/CMakeLists.txt': ['xray_xir_admission', 'xray_xir_declarations', 'xray_xir_scalar'],
    'src/execution/xir_runtime_host.cmake': ['xray_xir_runtime_host'],
}
GENERATED_RUNTIME_ROOTS = ['src/xir/xxir_equal.h', 'src/xir/xxir_panic.h', 'src/xir/xxir_atomic.h',
                           'src/xir/xxir_channel.h', 'src/xir/xxir_output.h', 'src/xir/xxir_nullable.h', 'src/shared/xr_error_core.h',
                           'src/execution/xr_xir_native_main.inc.c']

PLATFORMS = ('windows', 'posix')
# Each platform links its own system library set; POSIX hosts need only libm,
# which the C library may also provide.
SYSTEM_LIBRARIES = {'windows': ['kernel32', 'bcrypt', 'advapi32', 'userenv'], 'posix': ['m']}


def host_platform() -> str:
    return 'windows' if os.name == 'nt' else 'posix'


def archive_path(owner: str, platform: str) -> str:
    return f'lib/{owner}.lib' if platform == 'windows' else f'lib/lib{owner}.a'


def recipe(root: Path, platform: str | None = None) -> dict:
    platform = platform or host_platform()
    if platform not in PLATFORMS:
        raise ValueError(f'unsupported runtime platform: {platform}')
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
            if owner in ('xray_compile_resources', 'xray_xir_scalar'):
                branches = re.findall(r'if\(WIN32\)\s*target_sources\(' + owner +
                                      r'\s+PRIVATE\s+([^\n)]*)\)\s*else\(\)\s*target_sources\(' +
                                      owner + r'\s+PRIVATE\s+([^\n)]*)\)', text)
                if len(branches) != 1:
                    raise ValueError(f'exactly one platform runtime source branch required: {owner}')
                selected = branches[0][0 if platform == 'windows' else 1]
                groups[owner] += [(path.parent / spelling).resolve().relative_to(root).as_posix()
                                  for spelling in selected.split()]
    roots = [name for entries in groups.values() for name in entries] + GENERATED_RUNTIME_ROOTS
    pending = [root / name for name in roots]
    entries: dict[str, int] = {}
    while pending:
        path = pending.pop().resolve()
        relative = path.relative_to(root).as_posix()
        if relative in entries:
            continue
        if relative.startswith((
                'src/frontend/', 'src/vm/', 'src/program/', 'src/aot/',
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
        entries[archive_path(owner, platform)] = 5
    if len(entries) > 1024:
        raise ValueError('runtime recipe exceeds its file cap')
    return {'archive_source_groups': groups, 'system_libraries': SYSTEM_LIBRARIES[platform],
            'files': [{'path': name, 'kind': entries[name]} for name in sorted(entries)]}

def write_if_changed(path: Path, text: str) -> None:
    content = text.encode('utf-8')
    if path.is_file() and path.read_bytes() == content:
        return
    path.write_bytes(content)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--record')
    parser.add_argument('--depfile')
    parser.add_argument('--platform', choices=PLATFORMS, default=host_platform())
    args = parser.parse_args()
    result = recipe(Path(args.root), args.platform)
    lines = ['/* Generated from the actual runtime CMake source owners. */',
             'static const XrXirSdkRecipeFile sdk_recipe_files[] = {']
    lines.extend('    {' + json.dumps(entry['path']) + ', ' + str(entry['kind']) + 'u},'
                 for entry in result['files'])
    lines.append('};')
    write_if_changed(Path(args.output), '\n'.join(lines) + '\n')
    if args.record:
        write_if_changed(Path(args.record), json.dumps(result, indent=2) + '\n')
    if args.depfile:
        def escape(path: Path) -> str:
            return path.resolve().as_posix().replace('\\', '\\\\').replace('$', '$$').replace(
                ' ', '\\ ').replace('#', '\\#').replace(':', '\\:')
        root = Path(args.root).resolve()
        dependencies = [root / entry['path'] for entry in result['files'] if entry['kind'] != 5]
        dependencies += [root / name for name in OWNERS]
        dependencies.append(Path(__file__))
        target = Path(args.record or args.output)
        write_if_changed(Path(args.depfile), escape(target) + ': ' +
                         ' '.join(escape(path) for path in sorted(set(dependencies))) + '\n')

if __name__ == '__main__':
    main()
