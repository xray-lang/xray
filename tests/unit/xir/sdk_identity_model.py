"""Independent complete SDK framing and immutable retired packet fixtures."""
from __future__ import annotations
import hashlib
import json
import re
import struct
from pathlib import Path

PREFIX_NAMES = (
    'schema', 'wire', 'semantic', 'value_abi', 'call_abi', 'program_abi',
    'architecture', 'object_format', 'hosted', 'c_dialect', 'crt', 'sanitizers',
    'allocator', 'assertions', 'build_provider', 'abi_recipe_version',
    'closure_recipe_version',
)
CURRENT_PREFIX = (2, 34, 90, 28, 35, 37, 1, 1, 1, 11, 2, 0, 1, 0, 3, 1, 1)
CATALOG_SHA256 = '6110d54ab9c3645af0260a6f56c02af4fcdce8fb08bcb12cbc1864995fa30580'

def frame(manifest: dict) -> bytes:
    output = bytearray(b'xray:xir-runtime-sdk:v1')
    def word(value: int) -> None:
        output.extend(struct.pack('<I', value))
    def text(value: str) -> None:
        encoded = value.encode('utf-8')
        word(len(encoded))
        output.extend(encoded)
    for name in PREFIX_NAMES:
        word(manifest[name])
    for name in ('target_triple', 'abi_recipe', 'closure_recipe'):
        text(manifest[name])
    word(len(manifest['abi_measurements']))
    for row in manifest['abi_measurements']:
        word(row['id'])
        word(row['value'])
    word(len(manifest['files']))
    for row in manifest['files']:
        text(row['path'])
        word(row['kind'])
        output.extend(struct.pack('<Q', row['length']))
        output.extend(bytes.fromhex(row['sha256']))
    word(len(manifest['system_libraries']))
    for name in manifest['system_libraries']:
        text(name)
    return bytes(output)

def current_manifest(rows: list[dict]) -> dict:
    assert len(rows) == 504
    assert len({row['id'] for row in rows}) == 504
    assert rows == sorted(rows, key=lambda row: row['id'])
    manifest = dict(zip(PREFIX_NAMES, CURRENT_PREFIX))
    manifest.update(
        target_triple='x86_64-windows-msvc',
        abi_recipe='xray:xir-runtime-abi-measurements:v1',
        closure_recipe='xray:xir-runtime-recipe:windows-x86_64-hosted:v1',
        abi_measurements=[{'id': row['id'], 'value': row['value']} for row in rows],
        files=[{'path': 'lib/test.lib', 'kind': 5, 'length': 2,
                'sha256': hashlib.sha256(b'hi').hexdigest()}],
        system_libraries=['kernel32', 'bcrypt'],
    )
    return manifest

def original_arrays(header: bytes) -> dict[str, bytes]:
    pattern = rb'static const uint8_t (?:current_)?sdk_kat_(preimage|file_digest|digest)\[\]\s*=\s*\{(.*?)\};'
    result = {}
    for name, content in re.findall(pattern, header, re.S):
        result[name.decode()] = bytes(int(value, 16) for value in re.findall(rb'0x([0-9a-fA-F]{2})', content))
    assert set(result) == {'preimage', 'file_digest', 'digest'}
    return result

def retired_packets(root: Path) -> list[dict]:
    catalog_bytes = (root / 'sdk_retired_packets.json').read_bytes()
    assert hashlib.sha256(catalog_bytes).hexdigest() == CATALOG_SHA256
    catalog = json.loads(catalog_bytes)
    packets = catalog['packets']
    assert catalog['format'] == 1 and catalog['independent_of_current_layout_model']
    assert len(packets) == len({row['digest_hex'] for row in packets}) == 45
    assert sum(len(row['provenance']) for row in packets) == 123
    for packet in packets:
        header = (root / packet['original_header']).read_bytes()
        assert hashlib.sha256(header).hexdigest() == packet['header_sha256']
        git_bytes = b'blob ' + str(len(header)).encode() + b'\0' + header
        assert hashlib.sha1(git_bytes).hexdigest() == packet['git_blob']
        arrays = original_arrays(header)
        image = bytes.fromhex(packet['preimage_hex'])
        assert image == arrays['preimage'] == frame(packet['manifest'])
        assert hashlib.sha256(image).digest() == arrays['digest'] == bytes.fromhex(packet['digest_hex'])
        assert arrays['file_digest'] == bytes.fromhex(packet['file_digest_hex'])
        assert packet['manifest']['files'] == [{
            'path': 'lib/test.lib', 'kind': 5, 'length': 2,
            'sha256': packet['file_digest_hex'],
        }]
        assert arrays['file_digest'] == hashlib.sha256(b'hi').digest()
        ids = [row['id'] for row in packet['manifest']['abi_measurements']]
        assert len(ids) == len(set(ids))
    verify_original_relations(packets)
    return packets

def verify_original_relations(packets: list[dict]) -> None:
    def source_packet(label: str, name: str) -> dict:
        found = [row for row in packets if any(
            ref['label'] == label and ref['path'] == 'tests/unit/xir/' + name
            for ref in row['provenance'])]
        assert len(found) == 1
        return found[0]['manifest']
    # The original complete 351-row generations vary only in these two words.
    baseline = source_packet('P10', 'sdk_previous70_identity_golden.h')
    for semantic in range(66, 73):
        candidate = source_packet('P10', f'sdk_previous{semantic}_identity_golden.h')
        assert candidate['semantic'] == semantic
        normalized = dict(candidate, semantic=70, wire=25)
        assert normalized == baseline
    # These original complete generations retain their exact ABI table.
    before = source_packet('P10', 'sdk_previous75_identity_golden.h')
    after = source_packet('P10', 'sdk_previous76_identity_golden.h')
    assert before['wire'] == after['wire'] == 29
    assert dict(after, semantic=75) == before
    # Retired hash carrier layouts keep all bytes except their value generation.
    before = source_packet('P09', 'sdk_previous_value25_identity_golden.h')
    after = source_packet('P09', 'sdk_previous_value26_identity_golden.h')
    assert dict(after, value_abi=25) == before

def c_array(name: str, data: bytes) -> str:
    result = f'static const uint8_t {name}[] = {{\n'
    result += ''.join('    ' + ','.join(f'0x{byte:02x}' for byte in data[at:at + 16]) + ',\n'
                      for at in range(0, len(data), 16))
    return result + '};\n'

def current_header(manifest: dict) -> str:
    image = frame(manifest)
    assert len(image) == 4332
    result = '/* Independent complete current SDK framing and natural C layout facts. */\n'
    for name, data in (
        ('current_sdk_kat_preimage', image),
        ('current_sdk_kat_file_digest', hashlib.sha256(b'hi').digest()),
        ('current_sdk_kat_digest', hashlib.sha256(image).digest()),
    ):
        result += c_array(name, data)
    return result

def retired_sources(packets: list[dict]) -> dict[str, str]:
    output = {}
    index = (
        '/* Complete original SDK bytes retain distinct packet identities. */\n'
        'typedef struct SdkRetiredPacket {\n'
        '    const uint8_t *preimage, *digest, *file_digest;\n'
        '    size_t length;\n'
        '    const char *manifest;\n'
        '    size_t manifest_length;\n'
        '    uint32_t prefix[17];\n'
        '} SdkRetiredPacket;\n'
    )
    for ordinal, packet in enumerate(packets):
        base = f'sdk_retired_{ordinal}'
        name = f'sdk_retired/packet_{ordinal:02d}.inc.c'
        image = bytes.fromhex(packet['preimage_hex'])
        text = '/* Complete immutable packet; its original Git header is preserved separately. */\n'
        text += c_array(base + '_preimage', image)
        text += c_array(base + '_digest', bytes.fromhex(packet['digest_hex']))
        text += c_array(base + '_file_digest', bytes.fromhex(packet['file_digest_hex']))
        document = json.dumps(packet['manifest'], separators=(',', ':'), ensure_ascii=True)
        text += c_array(base + '_manifest', document.encode('ascii') + b'\0').replace(
            'static const uint8_t', 'static const char', 1)
        assert len(text.splitlines()) <= 3000
        output[name] = text
        index += f'#include "{name}"\n'
    index += 'static const SdkRetiredPacket sdk_retired_packets[] = {\n'
    for ordinal, packet in enumerate(packets):
        base = f'sdk_retired_{ordinal}'
        prefix = ','.join(str(packet['manifest'][name]) + 'u' for name in PREFIX_NAMES)
        index += (
            f'    {{{base}_preimage,{base}_digest,{base}_file_digest,sizeof({base}_preimage),'
            f'{base}_manifest,sizeof({base}_manifest)-1,{{{prefix}}}}},\n'
        )
    index += '};\n_Static_assert(sizeof(sdk_retired_packets)/sizeof(*sdk_retired_packets)==45,"complete retired SDK catalog");\n'
    output['sdk_retired_catalog.inc.c'] = index
    return output
