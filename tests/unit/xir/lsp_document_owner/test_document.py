from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
env = {**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())}
rows = []


def unpack(data):
    values = []
    while data:
        header, separator, tail = data.partition(b'\r\n\r\n')
        assert separator and re.fullmatch(rb'Content-Length: [0-9]+', header), data[:100]
        length = int(header.split(b': ')[1])
        assert 0 < length <= len(tail)
        values.append(json.loads(tail[:length].decode('utf-8')))
        data = tail[length:]
    return values


with tempfile.TemporaryDirectory(prefix='xray-document-文档-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="document-owner"\nversion="1.0.0"\nmain="root.xr"\n', encoding='utf-8')
    paths = [root / name for name in ('root.xr', 'lib.xr', 'extra.xr')]
    paths[0].write_text('import { value } from "./lib"\nexport fn main()->i64 { return value() }\n', encoding='utf-8')
    paths[1].write_text('export fn value()->i64 { return 1 }\n', encoding='utf-8')
    paths[2].write_text('export fn extra()->i64 { return 3 }\n', encoding='utf-8')
    modes = [(3, None)] if '--baseline-only' in sys.argv[3:] else [(i, None) for i in range(5)]
    for mode, fault in modes:
        args = [str(exe.resolve()), *(p.as_uri() for p in paths), str(mode)]
        if fault is not None:
            args.append(str(fault))
        child = subprocess.run(args,
                               input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=20)
        messages = unpack(child.stdout)
        for message in messages:
            assert message == {'jsonrpc': '2.0', 'method': 'textDocument/publishDiagnostics',
                               'params': {'uri': message['params']['uri'], 'diagnostics': []}}, message
            assert message['params']['uri'] in {p.as_uri() for p in paths[:2]}, message
        if mode == 4:
            Path('logs/baseline-clear-mode4.stderr.log').write_bytes(child.stderr)
            assert child.returncode != 0 and len(messages) == 1, (child.returncode, messages, child.stderr[-2000:])
            if exe.parent.name.endswith('-asan'):
                assert b'heap-use-after-free' in child.stderr, child.stderr[-2500:]
            rows.append({'mode': mode, 'exit': child.returncode, 'baseline': True,
                         'wire_messages': len(messages), 'expected_double_release_failure': True})
            continue
        expected = 1 if mode == 3 else 0
        assert child.returncode == expected, (mode, child.returncode, child.stderr[-2000:])
        if mode == 2:
            assert len(messages) == 2 and {m['params']['uri'] for m in messages} == {p.as_uri() for p in paths[:2]}
            clear = re.search(rb'CLEAR JSON=(\d+) failed=0', child.stderr)
            assert clear, child.stderr
            total = int(clear[1])
            modes.extend((5, p) for p in (0, total * 3 // 4, total - 1))
        elif mode == 5:
            assert len(messages) <= 1 and b'failed=1' in child.stderr, child.stderr
        else:
            assert not messages, (mode, messages)
        match = re.search(rb'RESULT mode=(\d+) closed_queue_detached=(\d+) table_free_drained=(\d+) rollbacks=(\d+) compiler_live=(\d+) json_live=(\d+) app_live=(\d+)', child.stderr)
        assert match and all(int(match[i]) == 0 for i in (5, 6, 7)), child.stderr[-1000:]
        assert [int(match[2]), int(match[3])] == ([0, 0] if mode == 3 else [1, 1])
        rows.append({'mode': mode, 'exit': child.returncode, 'closed_queue_detached': bool(int(match[2])),
                     'table_free_drained': bool(int(match[3])), 'rollbacks': int(match[4]),
                     'resource_blocks_zero': True, 'baseline': mode == 3,
                     'JSON_fault': fault, 'wire_messages': len(messages)})
        if mode == 1:
            prefixes = re.search(rb'PREFIXES Source=(\d+) focused=3 app=(\d+)', child.stderr)
            assert prefixes, child.stderr
            rows[-1].update({'Source_physical_prefixes': int(prefixes[1]), 'Source_focused_points': 3,
                             'all_open_app_prefixes': int(prefixes[2])})
        if mode == 2:
            rows[-1]['clear_JSON_prefixes'] = total
print(json.dumps({'cases': rows, 'actual_Source_and_document_transactions': True, 'full_FI': False}))
