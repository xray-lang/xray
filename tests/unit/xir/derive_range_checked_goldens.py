#!/usr/bin/env python3
"""Frame independent strict-range packets from named wire fields."""
from hashlib import sha256
from pathlib import Path
import struct

def words(*values):
    return struct.pack('<'+'I'*len(values),*values)

def instruction(opcode, result, first, count):
    return words(opcode,result,first,count,0,0) + struct.pack('<q',0) + words(0,0)

def packet(contract):
    body = words(0,1,0)                         # Program, one function, no declarations.
    body += words(5) + b'range'                # Exact function name.
    body += words(3,2,2,2,2)                   # Three i64 parameters and i64 result.
    body += words(1,0,2,0,0)                   # One block, two instructions, no protected frontier.
    body += words(2)
    body += instruction(148,0,0,3)            # Strict range consumes the three table operands.
    body += instruction(33,0,0,0)             # Return the first parameter.
    body += words(3,0,1,2)                     # Operand table count and exact IDs.
    body += words(0,0,0,0,0,0)                # Generics, types, nominals, interfaces, defaults, provenance.
    header = b'XRCHK\0\0\0' + words(25,contract,2,0,len(body),0)
    return header + sha256(header+body).digest() + body

def render():
    lines = ['/* Independently framed strict-range packets; no Xray writer is used. */',
             '#ifndef XIR_CHECKED_RANGE68_GOLDEN_H','#define XIR_CHECKED_RANGE68_GOLDEN_H']
    for name, contract in [('checked_range68_golden',68),('checked_range67_rejected',67)]:
        data = packet(contract)
        lines.append('static const uint8_t '+name+'[] = {')
        lines += ['    '+','.join('0x%02x'%b for b in data[i:i+12])+',' for i in range(0,len(data),12)]
        lines.append('};')
    return '\n'.join(lines+['#endif',''])

if __name__ == '__main__':
    path = Path(__file__).with_name('xir_checked_range68_golden.h')
    path.write_text(render(),encoding='utf-8',newline='\n')
