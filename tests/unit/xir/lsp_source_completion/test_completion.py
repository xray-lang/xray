from pathlib import Path
import json
import os
import re
import subprocess
import sys
import threading

exe = str(Path(sys.argv[1]).resolve())
full = os.environ.get('XR_LSP_COMPLETION_NORMAL_ONLY') is None


def unpack(data):
    values = []
    while data:
        head, sep, rest = data.partition(b'\r\n\r\n')
        assert sep and re.fullmatch(rb'Content-Length: [0-9]+', head), data[:100]
        length = int(head.split(b': ')[1])
        assert 0 < length <= len(rest)
        values.append(json.loads(rest[:length].decode('utf-8')))
        data = rest[length:]
    return values


def run(mode, fault=None):
    command = [exe, str(mode)] + ([] if fault is None else [str(fault)])
    if mode == 19:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        timer = threading.Timer(25, process.kill)
        timer.start()
        try:
            assert process.stderr.readline().strip() == b'READY'
            process.stdout.close()
            process.stdin.write(b'Content-Length: 2\r\n\r\n{}')
            process.stdin.flush()
            process.wait(timeout=25)
            process.stdin.close()
            output, error, code = b'', process.stderr.read(), process.returncode
        finally:
            timer.cancel()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    else:
        process = subprocess.run(command, input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25)
        output, error, code = process.stdout, process.stderr, process.returncode
    assert code == 0, (mode, fault, code, error[-2000:])
    match = re.search(rb'STATS mode=(\d+) status=(\w+) attempts=(\d+) injected=(\d+) producer=(\d+) admitted=(\d+) pending=(\d+) exit=(\d+) live=(\d+) bytes=(\d+)', error)
    assert match, error
    row = {name: int(match[i]) for i, name in enumerate(
        ['mode', 'status', 'attempts', 'injected', 'producer', 'admitted', 'pending', 'exit', 'live', 'bytes'], 1) if name != 'status'}
    row['status'] = match[2].decode()
    assert row['live'] == row['bytes'] == 0
    return unpack(output), row


def expected(mode, producer=False):
    if mode in (2, 10, 11, 12, 16, 17, 18, 19, 21, 22, 23, 24, 31, 32):
        return []
    identity = 'owned\0λ' if mode == 13 else -1 if mode == 14 else 2147483647 if mode == 15 else 0
    message = {'jsonrpc': '2.0', 'id': identity}
    if mode in (3, 4, 5, 29, 30):
        message['error'] = {'code': -32800, 'message': 'Request cancelled'}
    elif mode in (6, 26):
        message['error'] = {'code': -32803, 'message': 'Document state changed during Source query'}
    elif producer:
        message['error'] = {'code': -32603, 'message': 'Source query failed (stage=99 status=7)'}
    elif mode == 7:
        message['error'] = {'code': -32603, 'message': 'Source query failed (stage=6 status=3)'}
    elif mode == 8:
        message['error'] = {'code': -32603, 'message': 'Source query failed (stage=1 status=7)'}
    elif mode == 9:
        message['error'] = {'code': -32603, 'message': 'Source query returned no owned result'}
    elif mode in (27, 28):
        message['result'] = None if mode == 27 else []
    else:
        value = ''.join('\0' if i % 5 == 0 else '"' if i % 5 == 1 else '\\' if i % 5 == 2 else '\t' if i % 5 == 3 else 'x' for i in range(4096)) if mode == 33 else 'owned\0λ\\\n"'
        message['result'] = {'origin': 'captured', 'value': value}
    return [message]


statuses = {10: 'BAD_STAGE', 11: 'BAD_STAGE', 12: 'BAD_STRUCTURE', 16: 'BAD_STRUCTURE',
            17: 'BAD_STRUCTURE', 18: 'BAD_STRUCTURE', 19: 'IO', 21: 'BAD_STAGE',
            22: 'BAD_STRUCTURE', 23: 'BAD_STRUCTURE', 24: 'IO', 32: 'BAD_STAGE'}
rows = []
for mode in range(34):
    actual, row = run(mode)
    assert actual == expected(mode), (mode, actual, expected(mode))
    assert row['status'] == statuses.get(mode, 'OK'), row
    points = range(row['attempts']) if full else sorted({0, row['attempts'] // 2, row['attempts'] - 1}) if row['attempts'] else []
    for point in points:
        actual, fault = run(mode, point)
        assert fault['injected'] == 1, (mode, point, fault)
        assert fault['status'] in ('OK', 'OOM', 'IO', 'BAD_STAGE', 'BAD_STRUCTURE'), fault
        assert actual == (expected(mode, bool(fault['producer'])) if fault['status'] == 'OK' else []), (mode, point, actual, fault)
        assert fault['pending'] == (0 if fault['admitted'] or mode == 11 else 1), fault
        assert fault['exit'] == int(mode in (2, 31) or (fault['admitted'] and fault['status'] != 'OK')), fault
    rows.append({**row, 'tested_failure_points': len(points), 'full': full})
print(json.dumps({'cases': rows, 'full': full, 'wire': 'real stdio transport and complete LSP frames',
                  'Source_execution': 'NOT_RUN: terminal JSON/pending boundary; actual callback source is wired separately',
                  'compiler_resource_axes': 'N/A: pending operations allocate nothing; JSON publication uses JSON heap'}))
