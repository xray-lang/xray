from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
full = os.environ.get('XR_LSP_TRANSPORT_NORMAL_ONLY') is None


def frame(body):
    return b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body


def unpack(data):
    result = []
    while data:
        head, sep, tail = data.partition(b'\r\n\r\n')
        assert sep and re.fullmatch(rb'Content-Length: [0-9]+', head), data[:100]
        n = int(head.split(b': ')[1])
        assert len(tail) >= n
        result.append(tail[:n])
        data = tail[n:]
    return result


def stats(output):
    m = re.search(rb'STATS attempts=(\d+) injected=(\d+) live=(\d+) bytes=(\d+) peak=(\d+) frames=(\d+) connected=(\d+)', output)
    assert m, output
    result = dict(zip(['attempts', 'injected', 'live', 'bytes', 'peak', 'frames', 'connected'], map(int, m.groups())))
    assert result['live'] == result['bytes'] == 0
    return result


def run(mode, data=b'', fail=None, stream=None):
    command = [exe, mode] + ([] if fail is None else [str(fail)])
    p = subprocess.run(command, input=None if stream else data, stdin=stream,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25)
    assert p.returncode == 0, (mode, fail, p.returncode, p.stderr)
    return unpack(p.stdout), stats(p.stderr)


if '--strict' in sys.argv:
    cases = [b'X-Content-Length: 2\r\n\r\n{}',
             b'Content-Length: 2junk\r\n\r\n{}',
             b'Content-Length: 2\r\nContent-Length: 3\r\n\r\n{}']
    failures = []
    for wire in cases:
        bodies, row = run('relay', wire)
        if bodies:
            failures.append({'wire_hex': wire.hex(), 'accepted': [x.hex() for x in bodies], 'stats': row})
    print(json.dumps({'required_malformed_headers_rejected': len(cases) - len(failures),
                      'total': len(cases), 'failures': failures}))
    raise SystemExit(bool(failures))

rows = []


def check(name, mode, data, want, stream=None):
    offset = stream.tell() if stream else None
    bodies, row = run(mode, data, stream=stream)
    assert bodies == want, (name, bodies, want)
    if full:
        for i in range(row['attempts']):
            if stream:
                stream.seek(offset)
            actual, fault = run(mode, data, i, stream=stream)
            assert fault['injected'] == 1
            assert actual == want[:len(actual)], (name, i, actual)
    rows.append({'case': name, **row, 'full': full})


payload = '{"text":"λ😀"}'.encode() + b'\r\n\x1a'
check('byte_output', 'emit', b'', [payload])
check('null_output', 'invalid', b'', [])
check('oversize_output', 'oversize', b'', [])
# Keep the parent's input endpoint open while requiring an immediate return.
for mode in ['idle', 'buffered']:
    p = subprocess.Popen([exe, mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        p.wait(timeout=3)
        p.stdin.close()
        output, error = p.stdout.read(), p.stderr.read()
    except subprocess.TimeoutExpired:
        p.kill()
        output, error = p.communicate()
        raise AssertionError((mode, 'blocked with parent input open', error))
    assert p.returncode == 0 and not output, (mode, p.returncode, error)
    row = stats(error)
    assert row['connected'] == 1 and row['frames'] == (2 if mode == 'buffered' else 0)
    if full:
        for i in range(row['attempts']):
            bodies, fault = run(mode, fail=i)
            assert not bodies and fault['injected'] == 1
    rows.append({'case': mode + '_parent_pipe_open', **row, 'full': full})

check('coalesced_then_eof', 'relay', frame(b'{}') + frame(b'[]') + frame(payload), [b'{}', b'[]', payload])
check('empty_body', 'relay', frame(b''), [b''])
check('nul_body', 'relay', frame(b'\0\x1a\r\n'), [b'\0\x1a\r\n'])
check('growth', 'relay', frame(b'x' * 20000) + frame(b'{}'), [b'x' * 20000, b'{}'])
for length in [8191, 8192, 8193]:
    prefix = b'Content-Length: 2\r\nX-Filler: '
    wire = prefix + b'a' * (length - len(prefix) - 4) + b'\r\n\r\n{}'
    check('header_' + str(length), 'relay', wire, [b'{}'] if length <= 8192 else [])
for wire in [b'no separator', b'Unknown: 2\r\n\r\n{}', b'Content-Length: -1\r\n\r\n', b'Content-Length: 67108865\r\n\r\n']:
    check('rejected_' + wire[:16].decode(), 'relay', wire, [])

with tempfile.TemporaryFile() as stream:
    stream.write(frame(payload) + frame(b'[]'))
    stream.seek(0)
    check('regular_file', 'relay', None, [payload, b'[]'], stream=stream)

wire = frame(payload) + frame(b'[]')


def fragments(fail=2**64-1):
    p = subprocess.Popen([exe, 'fragment', str(fail), str(len(wire))], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    error = b''
    for byte in wire:
        ready = p.stderr.readline()
        error += ready
        if ready != b'READY\r\n' and ready != b'READY\n':
            break
        p.stdin.write(bytes([byte]))
        p.stdin.flush()
        step = p.stderr.readline()
        error += step
        if step != b'STEP\r\n' and step != b'STEP\n':
            break
    assert p.wait(timeout=4) == 0
    p.stdin.close()
    actual = unpack(p.stdout.read())
    return actual, stats(error + p.stderr.read())


actual, row = fragments()
assert actual == [payload, b'[]']
if full:
    for i in range(row['attempts']):
        actual, fault = fragments(i)
        assert fault['injected'] == 1 and actual == [payload, b'[]'][:len(actual)]
rows.append({'case': 'synchronized_byte_fragments', **row, 'full': full})

def broken(fail=None):
    p = subprocess.Popen([exe, 'broken'] + ([] if fail is None else [str(fail)]),
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    first = p.stderr.readline()
    if first.strip() == b'READY':
        p.stdout.close()
        p.stdin.write(b'x')
        p.stdin.flush()
    else:
        assert fail is not None, first
    assert p.wait(timeout=4) == 0
    p.stdin.close()
    return stats(first + p.stderr.read())


row = broken()
assert row['connected'] == 0
if full:
    for i in range(row['attempts']):
        fault = broken(i)
        assert fault['injected'] == 1 and fault['connected'] == 0
rows.append({'case': 'real_broken_output_pipe', **row, 'full': full})

for length in [67108863, 67108864, 67108865]:
    p = subprocess.Popen([exe, 'probe'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    p.stdin.write(b'Content-Length: ' + str(length).encode() + b'\r\n\r\n')
    p.stdin.flush()
    try:
        p.wait(timeout=3)
        p.stdin.close()
        error = p.stderr.read()
    except subprocess.TimeoutExpired:
        p.kill()
        _, error = p.communicate()
        raise AssertionError(('body limit probe blocked', length, error))
    assert p.returncode == 0, error
    row = stats(error)
    assert row['connected'] == int(length <= 67108864)
    if full:
        for i in range(row['attempts']):
            bodies, fault = run('probe', b'Content-Length: ' + str(length).encode() + b'\r\n\r\n', i)
            assert not bodies and fault['injected'] == 1
    rows.append({'case': 'body_limit_' + str(length), **row, 'full': full})

print(json.dumps({'cases': rows, 'full': full, 'compiler_resource_axes': 'NOT_APPLICABLE: transport receiving heap, original frame limits',
                  'scope': 'real Windows pipe and regular-file byte IO, buffered frames, app physical FI; strict framing requirement is a separate mandatory gate'}))
