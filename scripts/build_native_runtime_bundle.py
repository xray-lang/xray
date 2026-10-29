"""Publish canonical MSVC MT objects after the owning Ninja target completes.

Creation is private to native-runtime-bundle, whose actual target dependencies
finish first. Verification authenticates that publication and rejects subsequent
source, configuration, command, object or archive drift; it never builds inputs.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

VERSION_TEXT = ('set(PACKAGE_VERSION "1.0.0")\n'
                'if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n'
                '  set(PACKAGE_VERSION_EXACT TRUE)\n'
                '  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n'
                'endif()\n')
VERSION_BYTES = VERSION_TEXT.encode('ascii')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def record(path, name):
    require(path.is_file() and not path.is_symlink(), 'Missing or indirect input: ' + str(path))
    for part in (path, *path.parents):
        require(not part.is_symlink() and not getattr(part.lstat(), 'st_file_attributes', 0) & 0x400, 'Reparse input: ' + str(part))
    data = path.read_bytes()
    return dict(path=name, length=len(data), sha256=digest(data))


def run(arguments, directory):
    result = subprocess.run(list(map(str, arguments)), cwd=directory, capture_output=True, timeout=60)
    require(result.returncode == 0, 'Command failed: ' + repr(arguments) + '\n' + result.stderr.decode('utf-8', 'replace'))
    return result.stdout


def compiler_file(value):
    path = Path(value)
    if not path.is_absolute():
        require(value.lower() in ('cl', 'cl.exe'), 'Compiler cache needs an absolute file or bare cl')
        path = None
        for directory in os.environ.get('PATH', '').split(os.pathsep):
            directory = Path(directory.strip('"'))
            if not directory.is_absolute():
                continue
            candidate = directory/'cl.exe'
            if candidate.is_file():
                path = candidate
                break
        require(path is not None, 'Bare cl is absent from the explicit PATH')
    require(path.is_file(), 'Configured compiler file is missing: ' + str(path))
    return path


def object_facts(data):
    require(len(data) >= 20 and struct.unpack_from('<H', data)[0] == 0x8664, 'Expected x64 COFF')
    sections = struct.unpack_from('<H', data, 2)[0]
    optional = struct.unpack_from('<H', data, 16)[0]
    directives = b''
    for index in range(sections):
        offset = 20 + optional + index * 40
        require(offset + 40 <= len(data), 'Truncated COFF section table')
        if data[offset:offset+8].rstrip(b'\0') == b'.drectve':
            size, start = struct.unpack_from('<II', data, offset + 16)
            require(start + size <= len(data), 'Truncated COFF directives')
            directives += data[start:start+size]
    upper = directives.upper()
    require(re.search(rb'/DEFAULTLIB:"?LIBCMT(?:"|\s|$)', upper), 'COFF does not declare release MT')
    require(not re.search(rb'(?:MSVCRT|LIBCMTD|MSVCRTD)', upper), 'Mixed COFF CRT')
    require(not re.search(rb'_{2,3}(?:asan|ubsan|tsan|msan)[_A-Za-z]', data), 'Sanitized COFF')
    return dict(machine='COFF-x86_64', directives=directives.decode('ascii').strip(), sha256=digest(data))


def archive_facts(path, objects):
    data = path.read_bytes()
    require(data.startswith(b'!<arch>\n'), 'Expected static archive: ' + str(path))
    cursor, members, metadata, object_offsets = 8, [], [], []
    while cursor < len(data):
        require(cursor + 60 <= len(data), 'Truncated archive header')
        header = data[cursor:cursor+60]
        require(header[-2:] == b'`\n', 'Invalid archive header')
        size = int(header[48:58])
        body = data[cursor+60:cursor+60+size]
        require(len(body) == size, 'Truncated archive member')
        name = header[:16].rstrip()
        if name in (b'/', b'//'):
            require(not members and (name != b'//' or not any(x[0] == b'//' for x in metadata)), 'Unexpected archive metadata placement')
            metadata.append((name, body))
        else:
            facts = object_facts(body)
            object_offsets.append(cursor)
            members.append(dict(name=header[:16].decode('ascii').strip(), timestamp=header[16:28].decode('ascii').strip(), **facts))
        cursor += 60 + size + (size & 1)
    require([name for name,body in metadata] in ([b'/',b'/'],[b'/',b'/',b'//']), 'Unsupported archive metadata')
    first, second = metadata[0][1], metadata[1][1]
    require(len(first) >= 4 and len(second) >= 8, 'Truncated archive linker metadata')
    count = struct.unpack_from('>I', first)[0]
    require(4 + count*4 <= len(first), 'Truncated first linker offsets')
    offsets = struct.unpack_from('>' + str(count) + 'I', first, 4)
    require(all(offset in object_offsets for offset in offsets) and first[4+count*4:].count(b'\0') >= count, 'Invalid first linker member')
    count = struct.unpack_from('<I', second)[0]
    require(8 + count*4 <= len(second), 'Truncated second linker offsets')
    offsets = struct.unpack_from('<' + str(count) + 'I', second, 4)
    require(len(set(offsets)) == count and set(offsets) == set(object_offsets), 'Invalid second linker objects')
    symbols = struct.unpack_from('<I', second, 4+count*4)[0]
    require(8+count*4+symbols*2 <= len(second), 'Truncated second linker indices')
    indices = struct.unpack_from('<' + str(symbols) + 'H', second, 8+count*4)
    require(all(1 <= index <= count for index in indices) and second[8+count*4+symbols*2:].count(b'\0') >= symbols, 'Invalid second linker symbols')
    require(Counter(x['sha256'] for x in members) == Counter(x['sha256'] for x in objects),
            'Archive members do not equal actual target objects: ' + str(path))
    return members


def gather(args):
    root, build = args.root.resolve(), args.build.resolve()
    require(not run(['git', 'status', '--porcelain=v1', '--untracked-files=normal'], root).strip(), 'Native runtime publication requires clean source')
    commit = run(['git', 'rev-parse', 'HEAD'], root).decode().strip()
    inputs = json.loads(args.inputs.read_text(encoding='utf-8'))
    require(inputs['schema'] == 1, 'Unknown native runtime input schema')
    require((inputs['provider'], inputs['version'], inputs['profile']) == ('MSVC', '19.44.35219.0', 'Release'), 'Unqualified native runtime provider/profile')
    commands = json.loads((build/'compile_commands.json').read_text(encoding='utf-8'))
    cache = dict(re.findall(r'^([^#/:\n]+):[^=\n]+=(.*)$', (build/'CMakeCache.txt').read_text(encoding='utf-8'), re.M))
    require(cache['CMAKE_BUILD_TYPE'] == 'Release', 'Build cache profile mismatch')
    configured_compiler = compiler_file(cache['CMAKE_C_COMPILER'])
    ninja = cache['CMAKE_MAKE_PROGRAM']
    archives, effective, seen, compiler = [], {}, set(), None

    def source_identity(path):
        path = path.resolve()
        if path.is_relative_to(root):
            name = path.relative_to(root).as_posix()
        elif path.is_relative_to(build):
            name = '$build/' + path.relative_to(build).as_posix()
        else:
            # Toolchain headers remain provider inputs, never installed dependencies.
            name = '$provider/' + path.as_posix()
        facts = record(path, name)
        effective[name] = facts
        return facts

    for row in inputs['archives']:
        target, path = row['target'], Path(row['file']).resolve()
        require(target not in seen and row['crt'] == 'MultiThreaded', 'Duplicate target or non-MT target: ' + target)
        seen.add(target)
        require(path.is_relative_to(build) and path.suffix == '.lib', 'Archive outside build')
        pattern = re.compile(r'CMakeFiles[\\/]' + re.escape(target) + r'\.dir[\\/]')
        objects = []
        for command in commands:
            text = command['command']
            if not pattern.search(text):
                continue
            executable = re.match(r'^\s*(?:"([^"]+)"|(\S+))\s', text)
            require(executable is not None, 'Actual compiler command missing')
            selected = executable[1] or executable[2]
            selected = compiler_file(selected)
            require(selected.is_file() and selected.name.lower() == 'cl.exe', 'Actual compiler is not qualified MSVC')
            require(configured_compiler.samefile(selected), 'Configured compiler differs from actual object compiler')
            if compiler is None:
                compiler = selected
            require(compiler.samefile(selected), 'Mixed actual compiler executables')
            require(re.search(r'(?:^|\s)[/-]MT(?=\s|$)', text) and not re.search(r'(?:^|\s)[/-](?:MDd?|MTd)(?=\s|$)', text), 'Object CRT mismatch: ' + text)
            require('/DNDEBUG' in text and not re.search(r'fsanitize|/fp:fast|-ffast-math|/clang:', text, re.I), 'Unqualified object flags: ' + text)
            match = re.search(r'(?:^|\s)/Fo(?::)?("[^"]+"|\S+)', text)
            require(match is not None, 'Object producer missing /Fo')
            obj = Path(match[1].strip('"'))
            if not obj.is_absolute():
                obj = Path(command['directory']) / obj
            obj = obj.resolve()
            require(obj.is_relative_to(build), 'Object outside build')
            require(selected.stat().st_mtime_ns <= obj.stat().st_mtime_ns,
                    'Actual compiler is newer than object; a fresh build is required: ' + str(obj))
            facts = object_facts(obj.read_bytes())
            relative = obj.relative_to(build).as_posix()
            deps_raw = run([ninja, '-t', 'deps', relative], build).decode('utf-8')
            require(re.search(r'#deps\s+\d+.*\(VALID\)', deps_raw), 'Ninja dependencies missing or stale: ' + relative)
            deps = [line.strip() for line in deps_raw.splitlines()[1:] if line.startswith('    ') and line.strip()]
            require(deps, 'No actual header dependencies: ' + relative)
            dependencies = []
            for dependency in deps:
                item = Path(dependency)
                if not item.is_absolute():
                    item = build/item
                dependencies.append(source_identity(item))
                require(item.stat().st_mtime_ns <= obj.stat().st_mtime_ns, 'Object older than actual dependency: ' + str(item))
            source = Path(command['file'])
            source_record = source_identity(source)
            require(source.stat().st_mtime_ns <= obj.stat().st_mtime_ns, 'Object older than source')
            objects.append(dict(path=relative, source=source_record, command=text, dependencies=dependencies,
                                ninjaDeps=deps_raw, **facts))
        require(objects, 'No actual objects for ' + target)
        member_facts = archive_facts(path, objects)
        target_commands = run([ninja, '-t', 'commands', str(path.relative_to(build))], build).decode('utf-8')
        require(target_commands.strip(), 'Actual target commands missing')
        archives.append(dict(target=target, dependencies=row['dependencies'], file='lib/' + path.name, original=str(path),
                             actualTargetCommands=target_commands,
                             objects=objects, members=member_facts, **record(path, 'lib/' + path.name)))
    require(archives and archives[0]['target'] == 'xray_native_runtime', 'Native closure root missing')
    require(len({row['file'] for row in archives}) == len(archives), 'Archive output name collision')
    graph = {row['target']: row['dependencies'] for row in archives}
    reachable = set()

    def visit(target, ancestry):
        require(target in graph and target not in ancestry, 'Missing or cyclic native dependency: ' + target)
        if target in reachable:
            return
        reachable.add(target)
        for dependency in graph[target]:
            visit(dependency, ancestry | {target})

    visit('xray_native_runtime', set())
    require(reachable == set(graph), 'Unreachable native archive')
    for name in ('CMakeLists.txt', 'cmake/XrayNativeRuntime.cmake', 'cmake/XrayNativeRuntimeConfig.cmake.in',
                 'cmake/XrayNativeRuntimeInstall.cmake.in', 'cmake/xray_native_runtime_consumer.h.in',
                 'scripts/build_native_runtime_bundle.py', 'include/xray_native_runtime.h', 'include/xray_export.h'):
        source_identity(root/name)
    require(compiler is not None, 'Actual compiler identity missing')
    recipe = dict(schema=1, sourceCommit=commit, sourceDirty=False, provider='MSVC', version=inputs['version'],
                  profile='Release', crt='MT', sanitizers=0, compiler=record(compiler, compiler.name),
                  configuredCompiler=dict(value=cache['CMAKE_C_COMPILER'], file=str(configured_compiler),
                                          sha256=digest(configured_compiler.read_bytes())),
                  standardLibraries=cache.get('CMAKE_C_STANDARD_LIBRARIES', ''),
                  configuration={key: value for key,value in cache.items()
                      if key.startswith(('CMAKE_C_FLAGS', 'CMAKE_C_COMPILER', 'CMAKE_MSVC_RUNTIME_LIBRARY'))
                      and not key.endswith('-ADVANCED')},
                  target='x86_64-windows-msvc', archives=archives,
                  effectiveInputs=sorted(effective.values(), key=lambda x: x['path']))
    return recipe


def expected_manifest(args, recipe, recipe_path):
    root = args.root.resolve()
    files = [record(root/'include'/name, 'include/' + name)
             for name in ('xray_native_runtime.h', 'xray_export.h')]
    files.append(record(root/'cmake/xray_native_runtime_consumer.h.in', 'include/xray_native_runtime_consumer.h'))
    files.extend(record(Path(row['original']), row['file']) for row in recipe['archives'])
    files.append(record(recipe_path, 'native_runtime_recipe.json'))
    return dict(schema=1, namespace='xray-native-runtime-c', abi=1, component='XrayNativeRuntime',
                sourceCommit=recipe['sourceCommit'], sourceDirty=False, target=recipe['target'],
                provider=recipe['provider'], version=recipe['version'], profile='Release', crt='MT', sanitizers=0,
                archives=[row['file'] for row in recipe['archives']], files=files)


def bundle_inventory(output, manifest):
    expected = {row['path'] for row in manifest['files']} | {
        'native_runtime_manifest.json', 'XrayNativeRuntimeConfig.cmake',
        'XrayNativeRuntimeConfigVersion.cmake'}
    directories = {parent.as_posix() for name in expected for parent in Path(name).parents
                   if parent.as_posix() != '.'}
    actual = set()
    for path in output.rglob('*'):
        relative = path.relative_to(output).as_posix()
        require(not path.is_symlink() and not getattr(path.lstat(), 'st_file_attributes', 0) & 0x400,
                'Indirect bundle member: ' + relative)
        if path.is_dir():
            require(relative in directories, 'Unlisted bundle directory: ' + relative)
        else:
            require(path.is_file(), 'Nonregular bundle member: ' + relative)
            actual.add(relative)
    require(actual == expected, 'Unlisted or missing bundle members: ' + repr(sorted(actual ^ expected)))


def publish(args, recipe):
    output, root, build = args.output.resolve(), args.root.resolve(), args.build.resolve()
    require(output.is_relative_to(build) and output != build and not args.output.is_symlink(), 'Unsafe bundle destination')
    staged = Path(tempfile.mkdtemp(prefix='native-stage-', dir=build))
    files = []

    def copy(source, relative):
        destination = staged/relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        files.append(record(destination, relative))

    try:
        for name in ('xray_native_runtime.h', 'xray_export.h'):
            copy(root/'include'/name, 'include/' + name)
        copy(root/'cmake/xray_native_runtime_consumer.h.in', 'include/xray_native_runtime_consumer.h')
        for row in recipe['archives']:
            copy(Path(row['original']), row['file'])
        (staged/'native_runtime_recipe.json').write_text(json.dumps(recipe, indent=2) + '\n', encoding='utf-8')
        files.append(record(staged/'native_runtime_recipe.json', 'native_runtime_recipe.json'))
        manifest = expected_manifest(args, recipe, staged/'native_runtime_recipe.json')
        require(manifest['files'] == files, 'Copied bundle differs from canonical inputs')
        encoded = (json.dumps(manifest, indent=2) + '\n').encode()
        (staged/'native_runtime_manifest.json').write_bytes(encoded)
        configuration = (root/'cmake/XrayNativeRuntimeConfig.cmake.in').read_text(encoding='utf-8').replace('@MANIFEST_SHA256@', digest(encoded)).replace('@VERSION_SHA256@', digest(VERSION_BYTES))
        (staged/'XrayNativeRuntimeConfig.cmake').write_text(configuration, encoding='utf-8')
        (staged/'XrayNativeRuntimeConfigVersion.cmake').write_bytes(VERSION_BYTES)
        if output.exists():
            previous = Path(tempfile.mkdtemp(prefix='native-previous-', dir=build))
            previous.rmdir()
            output.rename(previous)
            try:
                staged.rename(output)
            except BaseException:
                previous.rename(output)
                raise
            require(previous.is_relative_to(build) and not previous.is_symlink(), 'Unsafe previous bundle')
            shutil.rmtree(previous)
        else:
            staged.rename(output)
        print('XrayNativeRuntime: ' + str(len(recipe['archives'])) + ' actual archives, clean ' + recipe['sourceCommit'])
    finally:
        if staged.exists():
            require(staged.is_relative_to(build) and not staged.is_symlink(), 'Unsafe staging directory')
            shutil.rmtree(staged)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'build', 'inputs', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--from-build-target', action='store_true')
    args = parser.parse_args()
    require(args.verify or args.from_build_target,
            'Publication requires the native-runtime-bundle Ninja dependency entry; standalone creation is unsupported')
    recipe = gather(args)
    if args.verify:
        version_path = args.output/'XrayNativeRuntimeConfigVersion.cmake'
        record(version_path, version_path.name)
        require(version_path.read_bytes() == VERSION_BYTES, 'Bundle package version identity mismatch')
        require(json.loads((args.output/'native_runtime_recipe.json').read_text(encoding='utf-8')) == recipe, 'Stale native runtime recipe')
        manifest_bytes = (args.output/'native_runtime_manifest.json').read_bytes()
        manifest = json.loads(manifest_bytes)
        require(manifest == expected_manifest(args, recipe, args.output/'native_runtime_recipe.json'), 'Bundle does not describe canonical inputs')
        bundle_inventory(args.output, manifest)
        for row in manifest['files']:
            require(record(args.output/row['path'], row['path']) == row, 'Bundle member mismatch')
        config = (args.root/'cmake/XrayNativeRuntimeConfig.cmake.in').read_text(encoding='utf-8').replace('@MANIFEST_SHA256@', digest(manifest_bytes)).replace('@VERSION_SHA256@', digest(VERSION_BYTES))
        require((args.output/'XrayNativeRuntimeConfig.cmake').read_text(encoding='utf-8') == config, 'Bundle configuration mismatch')
        require(manifest['sourceCommit'] == recipe['sourceCommit'] and manifest['archives'] == [x['file'] for x in recipe['archives']], 'Bundle order/source mismatch')
        print('XrayNativeRuntime bundle verified')
    else:
        publish(args, recipe)


if __name__ == '__main__':
    main()
