"""Encode the fixed identity fixture independently of the Checked writer."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys


def words(*values):
    return struct.pack('<'+'I'*len(values), *values)


def instruction(opcode, result=0, immediate=0):
    return words(opcode, result, 0, 0, 0, 0)+struct.pack('<q', immediate)+words(0, 0)


def function(name, result, instructions):
    name = name.encode('ascii')
    return (words(len(name))+name+words(0, result, 1, 0, len(instructions), 0, 0, len(instructions))+
            b''.join(instructions)+words(0))


def packet(role, timeout):
    unit = [instruction(25)]
    body = words(0, 4, 1)
    body += function('init', 0, unit)
    body += function('main', 2, [instruction(2, 2, 42), instruction(25)])
    body += function('test', 0, unit)
    body += function('unused_generic', 0, unit)
    body += words(1, 0, 0, 0, 1, 4)+b'root'+words(0, 0)
    for index in range(4):
        body += words(0, 0, 0, 0, 0, 0, 0, role if index == 2 else 0, timeout if index == 2 else 0)
    body += words(0, 1)  # No implementations; generics present.
    body += words(0, 0, 0)*3
    body += words(1, 0, 0, 0, 0)  # One ordinary unconstrained binder, no arguments.
    body += words(0, 0, 0, 0, 0)  # Types, nominal/interface pools, defaults, provenance.
    header = b'XRCHK\0\0\0'+words(24, 63, 2, 0)+struct.pack('<Q', len(body))
    return header+hashlib.sha256(header+body).digest()+body


driver, output = Path(sys.argv[1]), Path(sys.argv[2])
output.mkdir(parents=True, exist_ok=True)
subprocess.run([str(driver), str(output)], check=True)
for role, timeout in ((1, 0), (3, 0), (1, 7), (1, 8)):
    actual = (output/f'{role}-{timeout}.chk').read_bytes()
    expected = packet(role, timeout)
    assert actual == expected, (role, timeout, len(actual), len(expected))
print('four independent complete Checked24/63 identity packets PASS')
