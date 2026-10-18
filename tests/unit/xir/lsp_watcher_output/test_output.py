from pathlib import Path
import json
import os
import re
import subprocess
import sys
import threading

exe = str(Path(sys.argv[1]).resolve())
full = os.environ.get('XR_LSP_WATCHER_NORMAL_ONLY') is None


def unpack(data):
    result = []
    while data:
        head, sep, tail = data.partition(b'\r\n\r\n')
        assert sep and re.fullmatch(rb'Content-Length: [0-9]+', head), data[:100]
        n = int(head.split(b': ')[1])
        assert n <= len(tail)
        result.append(json.loads(tail[:n].decode('utf-8')))
        data = tail[n:]
    return result


def run(mode, fail=None):
    command = [exe, str(mode)] + ([] if fail is None else [str(fail)])
    if mode == 12:
        p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        timer = threading.Timer(25, p.kill)
        timer.start()
        try:
            assert p.stderr.readline().strip() == b'READY'
            p.stdout.close()
            p.stdin.write(b'Content-Length: 2\r\n\r\n{}')
            p.stdin.flush()
            p.wait(timeout=25)
            p.stdin.close()
            output, error, code = b'', p.stderr.read(), p.returncode
        finally:
            timer.cancel()
            if p.poll() is None:
                p.kill()
                p.wait(timeout=5)
    else:
        p = subprocess.run(command, input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25)
        output, error, code = p.stdout, p.stderr, p.returncode
    assert code == 0, (mode, fail, code, error[-2000:])
    m = re.search(rb'STATS mode=(\d+) status=(\w+) attempts=(\d+) injected=(\d+) before=(-?\d+) after=(-?\d+) exit=(\d+) sent=(\d+) live=(\d+) bytes=(\d+)', error)
    assert m, error
    row = {'mode': int(m[1]), 'status': m[2].decode(), 'attempts': int(m[3]), 'injected': int(m[4]),
           'before': int(m[5]), 'after': int(m[6]), 'exit': int(m[7]), 'sent': int(m[8]),
           'live': int(m[9]), 'bytes': int(m[10])}
    assert row['live'] == row['bytes'] == 0
    return unpack(output), row


params = {'registrations': [{'id': 'xray-file-watcher', 'method': 'workspace/didChangeWatchedFiles',
                            'registerOptions': {'watchers': [{'globPattern': '**/*.xr', 'kind': 7}]}}]}
statuses = {6: 'BAD_STRUCTURE', 7: 'BAD_STRUCTURE', 8: 'BUDGET', 10: 'IO', 11: 'IO', 12: 'IO',
            14: 'BUDGET', 15: 'BAD_STRUCTURE', 16: 'BAD_STRUCTURE', 18: 'BAD_STRUCTURE', 19: 'BAD_STRUCTURE'}


def expected(mode):
    if mode in statuses:
        return []
    if mode == 1:
        text = ''.join('\0' if i % 5 == 0 else '"' if i % 5 == 1 else '\\' if i % 5 == 2 else '\t' if i % 5 == 3 else 'x' for i in range(1024))
        return [{'jsonrpc': '2.0', 'id': 41, 'method': 'fixture/echo', 'params': {'echo': text}}]
    if mode in (2, 17):
        return [{'jsonrpc': '2.0', 'method': 'fixture/notify', 'params': params}]
    if mode == 3:
        return [{'jsonrpc': '2.0', 'id': 'owned\0λ', 'result': params}]
    if mode == 4:
        return [{'jsonrpc': '2.0', 'id': None, 'error': {'code': -32603, 'message': 'owned λ error'}}]
    if mode == 5:
        return [{'jsonrpc': '2.0', 'id': 1, 'method': 'client/registerCapability'}]
    ids = [1, 2] if mode == 13 else [2147483647] if mode == 9 else [-2147483647] if mode == 20 else [0] if mode == 21 else [1]
    return [{'jsonrpc': '2.0', 'id': i, 'method': 'client/registerCapability', 'params': params} for i in ids]


rows = []
for mode in range(22):
    want = expected(mode)
    actual, row = run(mode)
    assert actual == want, (mode, actual, want)
    assert row['status'] == statuses.get(mode, 'OK'), row
    assert row['exit'] == int(mode in (10, 11, 12))
    if full:
        for i in range(row['attempts']):
            prefix, fault = run(mode, i)
            assert fault['injected'] == 1 and fault['status'] == 'OOM', (mode, i, fault)
            assert prefix == want[:len(prefix)], (mode, i, prefix)
            assert fault['after'] == fault['before'] + fault['sent']
            assert fault['exit'] == int(mode == 11), (mode, i, fault)
    rows.append({**row, 'full': full})
print(json.dumps({'cases': rows, 'full': full, 'wire': 'real Windows stdio transport and strict LSP frames',
                  'compiler_resource_axes': 'NOT_APPLICABLE: JSON protocol allocation domain; transport receiving FI remains original90076'}))
