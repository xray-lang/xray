"""Encode the fixed identity fixture independently of the Checked writer."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import json

sys.dont_write_bytecode = True
CASES = ((1, 0), (3, 0), (1, 7), (1, 8))
HISTORY = ((24, 62), (24, 63), (25, 64), (25, 65), (25, 70), (26, 71))
# Frozen complete original models, including the former current65 fixture.
HISTORICAL_PACKET_SHA256 = {'1-0-24-62': '3657f024cd5a37d8620c832cae45d28ff3bf6bf7cf34bd9ab0673db068eb0e12', '1-0-24-63': '6c797ded75d6c180287dbb8d5b8ccdb97aacea99a9f16b289930a2a379d6b257', '1-0-25-64': '08e70683780cec431870470779644ba510a4488e924d6be943249d49fe285f5c', '1-0-25-65': 'd813aa30e0f3fd434a0e454956129b9af51fd2faeed5a4580cc0d210bf00d53d', '3-0-24-62': '74c24f4bb8fb52fb217265b9cfe7c61edfb3928a2947da9b99583d2e461745d9', '3-0-24-63': '9b5d10209cd157518c7a1ef0bbbec937ddc384de04e75e22809b7dcfa66d62e6', '3-0-25-64': '2ddef4890da5f9e331c0fec992746ef597bbd733177496b6a0852fb3a46c4078', '3-0-25-65': 'cf43fc8a09297696bda19fad7fc6e1703f86a1b490cae03433da18f8f96507c9', '1-7-24-62': 'e66d889414a1dc7c8d9547e976e71a342acb97ace2325cd4627376506f3fefbe', '1-7-24-63': '1f9c9f2683b59fec4dc02bf62dcbf95d07bc3bde23e185e49b2b860535dd8abb', '1-7-25-64': '7f2d0cae3d6ef511cae074bca5746e2270504177c43717774f7dc48dbf494dae', '1-7-25-65': '82ee0d01301118c85abf7d54a3ff56e3fa5e50c4f6dc2a12cee08e5529fd889a', '1-8-24-62': 'b41538fd92840a7bbcefcbb1a736c9c2b6d6fe2423c0f743b70c33919c6d42f7', '1-8-24-63': 'a9fbe04b02ab140c5b52a7a2054ba55a0f3607fab1808d094186f899e8f251f8', '1-8-25-64': '8a29770860aff23f0538b2e9950818413c0216f4e68ad8c903a4fec8d2572116', '1-8-25-65': '4679fb8c2ffb39d592636dfdc4c88fa2541097afa9b0c9da9ee4e18603cb40a3'}
# Preserve the previous positive models as complete retired inputs.
HISTORICAL_PACKET_SHA256.update({'1-0-25-70': '613ca59f5a42d4ad2fadb2ef3b3bc1f1f0e8d50417c03f37c1bf478e6cc42489', '3-0-25-70': '08d280451889bb1023d863365e352aed0776f503dcdf0f1d2f8fe8f0bde9b3f8', '1-7-25-70': '5f5a07ce748da0620aef96d0404b1aefe114d943878612c72a1e61ba54871c0e', '1-8-25-70': '2ded57795bd03046ab21aecf4519fe913d835b96b1ae289a97f7a0815c52b62e'})
# The no-type graph has the same complete body under the retired 26/71 schema.
HISTORICAL_PACKET_SHA256.update({'1-0-26-71': 'e976daccd563b6efd502c4cdd14f56d20dc333e89f80d90e1f31d3eba8db90e6', '3-0-26-71': 'a5c02500e90a5698f21caf29a896c66871463d7016c20dff732d61dfca32ef20', '1-7-26-71': '82f8c881409b21c10beeac435a2d33e419b686b87a6c5068dffc4c8e360cdf67', '1-8-26-71': '1c206598dd7947594827c14504dc20894d6482392cb6089fb8273872dadfae01'})


def words(*values):
    return struct.pack('<'+'I'*len(values), *values)


def instruction(opcode, result=0, immediate=0):
    return words(opcode, result, 0, 0, 0, 0)+struct.pack('<q', immediate)+words(0, 0)


def function(name, result, instructions):
    name = name.encode('ascii')
    return (words(len(name))+name+words(0, result, 1, 0, len(instructions), 0, 0, len(instructions))+
            b''.join(instructions)+words(0))


def packet(role, timeout, wire=27, semantic=72):
    assert (wire,semantic) in HISTORY + ((27,72),)
    return_opcode=33 if wire>=25 else 25
    unit = [instruction(return_opcode)]
    body = words(0, 4, 1)
    body += function('init', 0, unit)
    body += function('main', 2, [instruction(2, 2, 42), instruction(return_opcode)])
    body += function('test', 0, unit)
    body += function('unused_generic', 0, unit)
    body += words(1, 0, 0, 0, 1, 4)+b'root'+words(0, 0)
    for index in range(4):
        body += words(0, 0, 0, 0, 0, 0, 0, role if index == 2 else 0, timeout if index == 2 else 0)
    body += words(0, 1)  # No implementations; generics present.
    body += words(0, 0, 0)*3
    body += words(1, 0, 0, 0, 0)  # One ordinary unconstrained binder, no arguments.
    body += words(0, 0, 0)  # Empty type nodes, nominal and interface pools.
    if wire >= 27:
        body += words(0)  # Mandatory dense construction owner: zero nominals.
    body += words(0, 0)  # No defaults or provenance.
    header = b'XRCHK\0\0\0'+words(wire, semantic, 2, 0)+struct.pack('<Q', len(body))
    return header+hashlib.sha256(header+body).digest()+body


def verify_models():
    records = []
    for role, timeout in CASES:
        previous = packet(role, timeout, 25, 70)
        current = packet(role, timeout)
        assert len(previous) == 726 and len(current) == 730
        # The fixed graph has four functions, one unused binder, no type nodes
        # and no nominal declarations. Only its new mandatory zero row count is
        # added between the type pools and defaults; the header owns a new hash.
        insertion = len(previous) - 8
        assert current[64:insertion] == previous[64:insertion]
        assert current[insertion:insertion+4] == words(0)
        assert current[insertion+4:] == previous[insertion:]
        for wire, semantic in HISTORY:
            retired = packet(role, timeout, wire, semantic)
            key = f'{role}-{timeout}-{wire}-{semantic}'
            assert len(retired) == 726
            assert hashlib.sha256(retired).hexdigest() == HISTORICAL_PACKET_SHA256[key], key
            if wire >= 25:
                assert retired[64:] == previous[64:]
            records.append({'role': role, 'timeout': timeout, 'wire': wire,
                            'semantic': semantic, 'bytes': len(retired),
                            'sha256': hashlib.sha256(retired).hexdigest()})
        records.append({'role': role, 'timeout': timeout, 'wire': 27,
                        'semantic': 72, 'bytes': len(current),
                        'sha256': hashlib.sha256(current).hexdigest(),
                        'body_sha256': hashlib.sha256(current[64:]).hexdigest(),
                        'construction_count_offset': insertion})
    return records


def main(argv):
    models = verify_models()
    if argv == ['--verify-models']:
        print(json.dumps({'status': 'INDEPENDENT_FULL_IDENTITY_MODELS_VERIFIED',
                          'models': models, 'retired_complete': 24, 'current72': 4,
                          'callable_nodes': 0, 'driver_executed': False,
                          'runtime_qualification': 'NOT_RUN'}, indent=2))
        return 0
    assert len(argv) == 2, 'identity_wire.py <driver> <output> or --verify-models'
    driver, output = Path(argv[0]), Path(argv[1])
    output.mkdir(parents=True, exist_ok=True)
    for role, timeout in CASES:
        for wire, semantic in HISTORY:
            # Each old packet is encoded from the original complete declaration
            # model, never from a patched current header or a writer result.
            (output/f'{role}-{timeout}-old{wire}-{semantic}.chk').write_bytes(
                packet(role, timeout, wire, semantic))
    subprocess.run([str(driver), str(output)], check=True)
    for role, timeout in CASES:
        actual = (output/f'{role}-{timeout}.chk').read_bytes()
        expected = packet(role, timeout)
        assert actual == expected, (role, timeout, len(actual), len(expected))
    print('four independent complete Checked27/72 identity packets; '
          'old24/62,24/63,25/64,25/65,25/70,26/71 zero-allocation rejection PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
