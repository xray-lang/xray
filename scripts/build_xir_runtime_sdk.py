"""Build one closed host runtime bundle and its compiler-owned admission facts."""
from __future__ import annotations
import argparse, hashlib, json, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
from gen_xir_runtime_sdk_recipe import host_platform, recipe
from derive_xir_sdk_abi import prepare


def canonical_identity(manifest: dict) -> bytes:
    output=bytearray(b'xray:xir-runtime-sdk:v1')
    names=('schema','wire','semantic','value_abi','call_abi','program_abi','architecture',
           'object_format','hosted','c_dialect','crt','sanitizers','allocator','assertions',
           'build_provider','abi_recipe_version','closure_recipe_version')
    def word(value): output.extend(struct.pack('<I',value))
    def string(value):
        encoded=value.encode('utf-8');word(len(encoded));output.extend(encoded)
    for name in names:word(manifest[name])
    for name in ('target_triple','abi_recipe','closure_recipe'):string(manifest[name])
    word(len(manifest['abi_measurements']))
    for row in manifest['abi_measurements']:word(row['id']);word(row['value'])
    word(len(manifest['files']))
    for row in manifest['files']:
        string(row['path']);word(row['kind']);output.extend(struct.pack('<Q',row['length']))
        output.extend(bytes.fromhex(row['sha256']))
    word(len(manifest['system_libraries']))
    for name in manifest['system_libraries']:string(name)
    return hashlib.sha256(output).digest()


def version(root: Path, name: str) -> int:
    import re
    found=[]
    for header in ('xxir_checked.h','xxir_value.h','xxir_call.h','xxir_program.h'):
        found+=re.findall(r'^#define '+name+r'\s+(\d+)u?\s*$',
                          (root/'src/xir'/header).read_text(encoding='utf-8'),re.M)
    if len(found)!=1:raise ValueError('unique version definition required: '+name)
    return int(found[0])


# POSIX hosts publish their native object format against the system C library.
# The manifest words, triple and closure recipe must equal the compiler's
# XR_XIR_SDK_HOST_* facts for the same host.
POSIX_ARCHITECTURES={'x86_64':(1,'x86_64',0x3e,0x01000007),'arm64':(2,'aarch64',0xb7,0x0100000c)}


def posix_target(architecture: str) -> dict:
    if architecture not in POSIX_ARCHITECTURES:raise ValueError('unsupported runtime architecture: '+architecture)
    word,cpu,_,_=POSIX_ARCHITECTURES[architecture]
    darwin=sys.platform=='darwin'
    system='apple-darwin' if darwin else 'linux-gnu'
    return dict(architecture=word,object_format=3 if darwin else 2,crt=3,
                target_triple=cpu+'-'+system,
                closure_recipe='xray:xir-runtime-recipe:'+('darwin' if darwin else 'linux')+'-'+
                    architecture+'-hosted:v1')


def posix_image(image: bytes, architecture: str) -> None:
    _,_,elf_machine,macho_cpu=POSIX_ARCHITECTURES[architecture]
    if sys.platform=='darwin':
        assert struct.unpack_from('<II',image,0)==(0xfeedfacf,macho_cpu), 'ABI probe is not a native Mach-O image'
    else:
        assert image[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',image,18)[0]==elf_machine, \
            'ABI probe is not a native ELF64 image'


def build(root: Path, output: Path, compiler: str, crt: str, jobs: int, architecture: str) -> None:
    root=root.resolve();output=output.resolve();output.mkdir(parents=True,exist_ok=True)
    platform=host_platform();posix=platform=='posix'
    if posix!=(crt=='system'):raise ValueError('POSIX bundles use the system C library; Windows selects MT or MD')
    target=posix_target(architecture) if posix else None
    derived=recipe(root,platform);guards={}
    for row in derived['files']:
        if row['kind']==5:continue
        source=root/row['path'];data=source.read_bytes();guards[row['path']]=hashlib.sha256(data).digest()
        destination=output/row['path'];destination.parent.mkdir(parents=True,exist_ok=True)
        if not destination.exists() or destination.read_bytes()!=data:destination.write_bytes(data)
    with tempfile.TemporaryDirectory(prefix='xir-sdk-abi-',dir=output.parent) as temporary:
        proof=Path(temporary)/'independent';prepare(proof)
        expected=json.loads((proof/'EXPECTED.json').read_text(encoding='utf-8'))['rows']
        assert expected and len({row['id'] for row in expected})==len(expected)
        shutil.copy2(proof/'abi_probe.c',output/'abi_probe.c')
        (output/'abi-expected.json').write_text(json.dumps(expected,indent=2)+'\n',encoding='utf-8')
    lines=['cmake_minimum_required(VERSION 3.25)','project(XirRuntimeSdk C)',
           'set(CMAKE_C_STANDARD 11)','set(CMAKE_C_STANDARD_REQUIRED ON)',
           'set(CMAKE_C_EXTENSIONS OFF)']
    if posix:lines+=[
           'if(WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)',
           '  message(FATAL_ERROR "This runtime bundle requires a hosted 64-bit POSIX target")','endif()',
           # The same feature macros the compiler build gives these sources.
           'add_compile_definitions(NDEBUG)',
           'if(APPLE)','  add_compile_definitions(_POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)',
           'else()','  add_compile_definitions(_GNU_SOURCE)','endif()',
           'add_compile_options(-Wall -Wextra -Werror -ffp-contract=off)']
    else:lines+=[
           'if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)',
           '  message(FATAL_ERROR "This runtime bundle requires hosted Windows x86_64")','endif()',
           'add_compile_definitions(NDEBUG NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)',
           'if(MSVC)','  add_compile_options(/W4 /WX /utf-8)',
           'else()','  add_compile_options(-Wall -Wextra -Werror)','endif()',
           'if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")','  add_compile_options(/experimental:c11atomics)','endif()',
           'if(CMAKE_C_COMPILER_ID STREQUAL "Clang")',
           '  if(MSVC)','    add_compile_options(/clang:-ffp-contract=off)',
           '  else()','    add_compile_options(-ffp-contract=off)','  endif()','endif()']
    lines+=['include_directories(src include generated)',
           'set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib")',
           'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/compiler-id.txt" "${CMAKE_C_COMPILER_ID}")']
    for name,sources in derived['archive_source_groups'].items():
        lines.append('add_library('+name+' STATIC '+' '.join(sources)+')')
    archives=' '.join(derived['archive_source_groups'])
    lines.append('add_executable(abi_probe abi_probe.c)')
    if posix:
        # Admission and declaration archives reference each other; single-pass
        # ELF linkers need an explicit group, while Darwin rescans archives.
        lines+=['if(APPLE)','  target_link_libraries(abi_probe PRIVATE '+archives+')','else()',
                '  target_link_libraries(abi_probe PRIVATE -Wl,--start-group '+archives+' -Wl,--end-group)',
                'endif()','target_link_libraries(abi_probe PRIVATE '+' '.join(derived['system_libraries'])+')']
    else:lines.append('target_link_libraries(abi_probe PRIVATE '+archives+' '+' '.join(derived['system_libraries'])+')')
    (output/'CMakeLists.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    builddir=output/'build'
    configure=['cmake','-S',str(output),'-B',str(builddir),'-G','Ninja',
               '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER='+compiler]
    if posix:
        if sys.platform=='darwin':configure.append('-DCMAKE_OSX_ARCHITECTURES='+architecture)
    else:configure.append('-DCMAKE_MSVC_RUNTIME_LIBRARY='+('MultiThreadedDLL' if crt=='MD' else 'MultiThreaded'))
    subprocess.run(configure,check=True)
    subprocess.run(['cmake','--build',str(builddir),'-j',str(jobs)],check=True)
    probe=builddir/('abi_probe' if posix else 'abi_probe.exe')
    image=probe.read_bytes()
    if posix:posix_image(image,architecture)
    else:
        pe=struct.unpack_from('<I',image,60)[0]
        assert image[pe:pe+4]==b'PE\0\0' and struct.unpack_from('<H',image,pe+4)[0]==0x8664
    measured=json.loads(subprocess.check_output([str(probe)]))
    assert measured==[{'id':row['id'],'value':row['value']} for row in expected], 'runtime ABI oracle mismatch'
    provider={'Clang':1,'AppleClang':1,'GNU':2,'MSVC':3}.get((builddir/'compiler-id.txt').read_text().strip())
    if provider is None:raise ValueError('unqualified archive provider')
    manifest=dict(schema=2,wire=version(root,'XR_XIR_CHECKED_SCHEMA'),
                  semantic=version(root,'XR_XIR_CHECKED_CONTRACT'),
                  value_abi=version(root,'XR_XIR_VALUE_ABI_VERSION'),
                  call_abi=version(root,'XR_XIR_CALL_ABI_VERSION'),
                  program_abi=version(root,'XR_XIR_PROGRAM_ABI_VERSION'),
                  architecture=target['architecture'] if posix else 1,
                  object_format=target['object_format'] if posix else 1,hosted=1,c_dialect=11,
                  crt=3 if posix else 2 if crt=='MD' else 1,sanitizers=0,allocator=1,assertions=0,
                  build_provider=provider,abi_recipe_version=1,closure_recipe_version=1,
                  target_triple=target['target_triple'] if posix else 'x86_64-windows-msvc',
                  abi_recipe='xray:xir-runtime-abi-measurements:v1',
                  closure_recipe=target['closure_recipe'] if posix else
                      'xray:xir-runtime-recipe:windows-x86_64-hosted:v1',
                  abi_measurements=measured,files=[],system_libraries=derived['system_libraries'])
    for row in derived['files']:
        data=(output/row['path']).read_bytes()
        manifest['files'].append(dict(**row,length=len(data),sha256=hashlib.sha256(data).hexdigest()))
    assert all(hashlib.sha256((root/path).read_bytes()).digest()==digest for path,digest in guards.items())
    identity=canonical_identity(manifest)
    header=['/* Same-source bundle facts, measured against an independent ABI oracle. */',
            'static const uint32_t sdk_expected_prefix[17] = {'+
            ','.join(str(manifest[name])+'u' for name in list(manifest)[:17])+'};',
            'static const uint8_t sdk_expected_identity[32] = {'+','.join(str(byte) for byte in identity)+'};']
    (output/'sdk_generated.h').write_text('\n'.join(header)+'\n',encoding='utf-8')
    (output/'sdk_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(identity=identity.hex(),files=len(manifest['files']),
                         archive_sources=derived['archive_source_groups'],provider=provider,crt=crt)))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--compiler',required=True)
    parser.add_argument('--crt',choices=('MT','MD','system'),default='MD' if host_platform()=='windows' else 'system')
    parser.add_argument('--architecture',default='x86_64',help='POSIX target architecture: x86_64 or arm64')
    parser.add_argument('--jobs',type=int,default=4);args=parser.parse_args()
    if not 1<=args.jobs<=64:parser.error('jobs must be between 1 and 64')
    build(args.root,args.output,args.compiler,args.crt,args.jobs,args.architecture)


if __name__=='__main__':main()
