"""Publish the explicit public Source API archive closure as a separate SDK."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import tempfile


def build_bundle(root, build, output, args):
    archives = json.loads(args.inputs.read_text(encoding='utf-8'))['archives']
    commands = json.loads((build/'compile_commands.json').read_text(encoding='utf-8'))
    assert len(archives) == len({row['target'] for row in archives}) and archives[0]['name'] == 'xray_source_api'
    assert not any(row['name'] in ('xray_core', 'xray_core_objs', 'xray_runtime', 'xray_api') for row in archives)
    recipe, members = [], []
    output.mkdir(parents=True, exist_ok=True)

    def publish(source, relative):
        destination = output/relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        data = destination.read_bytes()
        members.append(dict(path=relative, length=len(data), sha256=hashlib.sha256(data).hexdigest()))

    for row in archives:
        archive = Path(row['file']).resolve()
        assert archive.is_relative_to(build) and archive.is_file()
        pattern = re.compile(r'CMakeFiles[\\/]'+re.escape(row['target'])+r'\.dir[\\/]')
        sources = []
        for command in commands:
            if not pattern.search(command['command']):
                continue
            assert re.search(r'(?:^|\s)[/-]MD(?:\s|$)', command['command']), command['file']
            assert not re.search(r'(?:^|\s)[/-](?:MT|MDd|MTd)(?:\s|$)', command['command'])
            assert 'fsanitize=' not in command['command'], command['file']
            source = Path(command['file']).resolve()
            name = '$build/'+source.relative_to(build).as_posix() if source.is_relative_to(build) else source.relative_to(root).as_posix()
            data = source.read_bytes()
            sources.append(dict(path=name, length=len(data), sha256=hashlib.sha256(data).hexdigest()))
        assert sources and len(sources) == len({source['path'] for source in sources}), row['target']
        relative = 'lib/'+row['name']+archive.suffix
        publish(archive, relative)
        recipe.append(dict(target=row['name'], object_target=row['target'], archive=relative, sources=sources))
    for name in ('xray_source.h', 'xray_export.h', 'xray_source_ast.h', 'xray_source_value.h', 'xray_source_entry.h', 'xray_source_instance.h'):
        publish(root/'include'/name, 'include/'+name)
    record = dict(schema=1, purpose='explicit Source compiler and VM embedding; separate from AOT runtime',
                  archives=recipe, system_libraries=['kernel32'])
    recipe_path = output/'source_sdk_recipe.json'
    recipe_path.write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    data = recipe_path.read_bytes()
    members.append(dict(path=recipe_path.name, length=len(data), sha256=hashlib.sha256(data).hexdigest()))
    api_versions = re.findall(r'^#define XRAY_SOURCE_API_VERSION (\d+)u$',
        (root/'include/xray_source.h').read_text(encoding='utf-8'), re.M)
    assert len(api_versions) == 1
    manifest = dict(schema=1, api_version=int(api_versions[0]), target='windows-x86_64', crt='MD', sanitizers=0,
                    provider=args.provider, profile=args.profile, archives=[row['archive'] for row in recipe], files=members)
    manifest_bytes = (json.dumps(manifest, indent=2)+'\n').encode('utf-8')
    configuration = (root/'cmake/XraySourceConfig.cmake.in').read_text(encoding='utf-8')
    configuration = configuration.replace('@XR_SOURCE_MANIFEST_SHA256@', hashlib.sha256(manifest_bytes).hexdigest())
    (output/'XraySourceConfig.cmake').write_text(configuration, encoding='utf-8')
    (output/'source_sdk_manifest.json').write_bytes(manifest_bytes)
    print('Source SDK: '+str(len(recipe))+' explicit archives, '+str(sum(len(row['sources']) for row in recipe))+' actual source objects')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'build', 'inputs', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--provider', required=True)
    parser.add_argument('--profile', required=True)
    args = parser.parse_args()
    root, build, publication = args.root.resolve(), args.build.resolve(), args.output.resolve()
    assert publication.is_relative_to(build) and publication != build and not args.output.is_symlink()
    publication.parent.mkdir(parents=True, exist_ok=True)
    staged = Path(tempfile.mkdtemp(prefix='source-sdk-stage-', dir=publication.parent))
    backup = None
    try:
        build_bundle(root, build, staged, args)
        if publication.exists():
            backup = Path(tempfile.mkdtemp(prefix='source-sdk-previous-', dir=publication.parent))
            backup.rmdir()
            assert publication.resolve().is_relative_to(build) and backup.resolve().is_relative_to(build)
            publication.rename(backup)
        try:
            assert staged.resolve().is_relative_to(build) and publication.resolve().is_relative_to(build)
            staged.rename(publication)
        except BaseException:
            if backup is not None:
                backup.rename(publication)
                backup = None
            raise
        if backup is not None:
            assert backup.resolve().is_relative_to(build) and not backup.is_symlink()
            shutil.rmtree(backup)
    finally:
        if staged.exists():
            assert staged.resolve().is_relative_to(build) and not staged.is_symlink()
            shutil.rmtree(staged)


if __name__ == '__main__':
    main()
