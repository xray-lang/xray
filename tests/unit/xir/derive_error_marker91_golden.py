"""Derive one complete legal ERROR definition packet from explicit wire facts.

The literal module has one function e<T: ERROR>(T) -> I64 with THROW arg0.
All absent tables are explicit. Inputs are only the constants below, checked
against the fixed canonical field format; no product code, previous golden,
compiler executable, or emitted packet is read or imported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


# Each row is a complete wire field, rather than a patch to an older packet.
BODY_FIELDS = (
    ('module.linkage', 'I', 0),
    ('module.function_count', 'I', 1),
    ('module.declarations_present', 'I', 0),
    ('function.name_length', 'I', 1),
    ('function.name', 'bytes', b'e'),
    ('function.parameter_count', 'I', 1),
    ('function.parameter[0].type', 'I', 65536),
    ('function.result', 'I', 2),
    ('function.block_count', 'I', 1),
    ('block.first', 'I', 0),
    ('block.count', 'I', 1),
    ('block.panic', 'I', 0),
    ('block.frontier', 'I', 0),
    ('function.instruction_count', 'I', 1),
    ('instruction.op', 'I', 30),
    ('instruction.type', 'I', 0),
    ('instruction.args[0]', 'I', 0),
    ('instruction.args[1]', 'I', 0),
    ('instruction.targets[0]', 'I', 0),
    ('instruction.targets[1]', 'I', 0),
    ('instruction.immediate', 'Q', 0),
    ('instruction.type_arguments[0]', 'I', 0),
    ('instruction.type_arguments[1]', 'I', 0),
    ('function.operand_count', 'I', 0),
    ('module.generics_present', 'I', 1),
    ('generic.parameter_count', 'I', 1),
    ('generic.kinds_present', 'I', 0),
    ('constraint.markers', 'I', 2),
    ('constraint.interface_count', 'I', 0),
    ('generic.argument_count', 'I', 0),
    ('types.node_count', 'I', 0),
    ('types.nominal_count', 'I', 0),
    ('types.interface_count', 'I', 0),
    ('construction.nominal_count', 'I', 0),
    ('construction.materialization_count', 'I', 0),
    ('construction.comptime.site_count', 'I', 0),
    ('construction.comptime.binding_count', 'I', 0),
    ('defaults.count', 'I', 0),
    ('provenance.kind', 'I', 0),
)


def encode_fields(rows, start):
    data = bytearray()
    fields = []
    for name, kind, value in rows:
        raw = value if kind == 'bytes' else struct.pack('<' + kind, value)
        fields.append(dict(name=name, offset=start + len(data), bytes=len(raw),
                           encoding='raw' if kind == 'bytes' else 'little-endian',
                           value=raw.hex() if kind == 'bytes' else value))
        data.extend(raw)
    return bytes(data), fields


def derive():
    body, fields = encode_fields(BODY_FIELDS, 64)
    prefix, header_fields = encode_fields((
        ('header.magic', 'bytes', b'XRCHK\0\0\0'),
        ('header.schema', 'I', 32),
        ('header.contract', 'I', 91),
        ('header.stage', 'I', 2),
        ('header.reserved', 'I', 0),
        ('header.payload_length', 'Q', len(body)),
    ), 0)
    digest = hashlib.sha256(prefix + body).digest()
    packet = prefix + digest + body
    offsets = {row['name']: row['offset'] for row in fields}
    assert len(prefix) == 32 and len(body) == 157 and len(packet) == 221
    assert offsets['generic.kinds_present'] == 169
    assert offsets['constraint.markers'] == 173
    fields = header_fields + [dict(name='header.digest', offset=32, bytes=32,
                                  encoding='SHA256(packet[0:32]+packet[64:])',
                                  value=digest.hex())] + fields
    end = 0
    for field in fields:
        assert field['offset'] == end
        end += field['bytes']
    assert end == len(packet)
    return packet, fields


def render_header(packet):
    lines = [
        '/*',
        ' * xray - Lightweight typed scripting with native concurrency',
        ' * https://www.xray-lang.org',
        ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
        ' * Licensed under the MIT License',
        ' *',
        ' * xir_error_marker91_golden.h - Independent legal ERROR definition packet',
        ' *',
        ' * KEY CONCEPT:',
        ' *   Explicit literal fields and SHA256 frame one complete current packet.',
        ' *   Regenerate with derive_error_marker91_golden.py; no writer is an input.',
        ' */',
        '#ifndef XIR_ERROR_MARKER91_GOLDEN_H',
        '#define XIR_ERROR_MARKER91_GOLDEN_H',
        '#include <stdint.h>',
        'static const uint8_t error_marker91_golden[] = {',
    ]
    for start in range(0, len(packet), 12):
        lines.append('    ' + ', '.join(f'0x{byte:02x}' for byte in packet[start:start + 12]) + ',')
    lines.extend((
        '};',
        '#define ERROR_MARKER91_KINDS_PRESENT 169u',
        '#define ERROR_MARKER91_MARKER 173u',
        '#define ERROR_MARKER91_PAYLOAD_LENGTH 157u',
        '#define ERROR_MARKER91_PACKET_LENGTH 221u',
        '#endif // XIR_ERROR_MARKER91_GOLDEN_H',
        '',
    ))
    return '\n'.join(lines).encode('ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--output-root', type=Path,
                        help='Optional separate directory for the derived header')
    parser.add_argument('--field-table', type=Path)
    args = parser.parse_args()
    packet, fields = derive()
    header = render_header(packet)
    folder = args.output_root if args.output_root else Path(__file__).parent
    output = folder / 'xir_error_marker91_golden.h'
    if args.write:
        folder.mkdir(parents=True, exist_ok=True)
        output.write_bytes(header)
    elif not output.is_file() or output.read_bytes() != header:
        raise SystemExit('Independent ERROR golden differs; regenerate explicitly with --write')
    summary = dict(status='INDEPENDENT_COMPLETE_LEGAL_ERROR_DEFINITION',
                   schema=32, contract=91, packet_bytes=len(packet),
                   payload_bytes=len(packet) - 64, digest=packet[32:64].hex(),
                   packet_sha256=hashlib.sha256(packet).hexdigest(),
                   payload_sha256=hashlib.sha256(packet[64:]).hexdigest(),
                   header_sha256=hashlib.sha256(header).hexdigest(),
                   complete_field_coverage=True, writer_or_executable_used=False)
    if args.field_table:
        args.field_table.write_text(json.dumps(dict(**summary, fields=fields), indent=2) + '\n',
                                   encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
