from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
mode = sys.argv[3]
assert mode in ('--normal', '--capture', '--counterfactual')
digest = hashlib.sha256(exe.read_bytes()).hexdigest()


def unpack(data):
    values = []
    while data:
        header, separator, rest = data.partition(b'\r\n\r\n')
        assert separator and re.fullmatch(rb'Content-Length: [0-9]+', header), data[:100]
        size = int(header.split(b': ')[1])
        assert 0 < size <= len(rest)
        values.append(json.loads(rest[:size].decode('utf-8')))
        data = rest[size:]
    return values


rows = []
text = ('fn target(v:i64)->i64{return v}\n'
        'fn wrapper(v:i64)->i64{return target(v)+target(v)}\n'
        'const answer=wrapper(1)\n'
        'interface Base {}\ninterface Child extends Base {}\n')
with tempfile.TemporaryDirectory(prefix='hierarchy-task-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="hierarchy-task"\nmain="root.xr"\n', encoding='utf-8')
    entry, library = root / 'root.xr', root / 'lib.xr'
    library.write_text('export fn other()->i64{return 1}\n', encoding='utf-8')
    (root / 'ignored.xr').write_text('deliberately invalid ignored source', encoding='utf-8')
    (root / 'skip').mkdir()
    (root / 'skip/bad.xr').write_text('deliberately invalid ignored directory source', encoding='utf-8')
    for method in range(4, 10):
        scenarios = [11] if mode == '--counterfactual' else [10] if mode == '--capture' else range(10)
        for scenario in scenarios:
            entry.write_text(text if scenario == 3 else 'invalid disk text replaced by editor overlay', encoding='utf-8')
            result = subprocess.run([str(exe.resolve()), entry.as_uri(), library.as_uri(), str(root.resolve()),
                                     str(scenario), str(method)], input=b'', stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=20,
                                    env={**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())})
            if mode == '--counterfactual':
                assert result.returncode == 1 and not result.stdout, (method, result.returncode, result.stderr[-2000:])
                marker = f'BEFORE actual SourceTask rejected hierarchy mode={method} status=1 physical0/0 JSON0/0 app0/0'
                assert result.stderr.count(marker.encode()) == 1, result.stderr
                rows.append(dict(method=method, old_actual_task_rejected=True))
                continue
            assert result.returncode == 0, (method, scenario, result.returncode, result.stderr[-2500:])
            marker = f'HIERARCHY method={method} scenario={scenario} '
            assert result.stderr.count(marker.encode()) == 1 and b'physical0/0 JSON0/0 app0/0 producers_dead=1' in result.stderr
            assert f'exit={1 if scenario in (5, 6) else 0}'.encode() in result.stderr, result.stderr
            messages = unpack(result.stdout)
            if scenario in (5, 6, 9):
                assert messages == [], (method, scenario, messages)
            else:
                assert len(messages) == 1 and messages[0]['id'] == 'source\0λ' and messages[0]['jsonrpc'] == '2.0'
                message = messages[0]
                if scenario in (1, 2):
                    assert message['error']['code'] == -32800, message
                elif scenario == 4:
                    assert message['error']['code'] == -32803, message
                elif scenario == 8:
                    assert message['error']['code'] == -32603, message
                else:
                    assert 'error' not in message, (method, scenario, message)
                    values = message['result']
                    assert len(values) == 1, values
                    item = values[0].get('from', values[0].get('to', values[0]))
                    expected = {4: 'target', 5: 'wrapper', 6: 'target', 7: 'Child', 8: 'Base', 9: 'Child'}[method]
                    assert item['name'] == expected and item['uri'] == entry.as_uri(), item
                    if method in (5, 6):
                        assert len(values[0]['fromRanges']) == 2, values
                    version = item['data']['version']
                    assert version is None if scenario == 3 else version == '7', item
            row = dict(method=method, scenario=scenario, wire_messages=len(messages), all_producers_destroyed=True)
            if scenario == 2:
                interior = re.search(rb'INTERIOR mode=(\d+) trigger=(\d+) calls=(\d+)', result.stderr)
                assert interior and int(interior[1]) == method and int(interior[2]) <= int(interior[3]) <= int(interior[2]) + 64
                row.update(cancel_trigger=int(interior[2]), work_calls=int(interior[3]))
            if scenario == 10:
                prefix = re.search(rb'HIERARCHY_CAPTURE method=(\d+) prefixes=(\d+) app_prefixes=3 physical0/0', result.stderr)
                assert prefix and int(prefix[1]) == method and int(prefix[2]) > 0, result.stderr
                row['capture_FI_prefixes'] = int(prefix[2])
                row['app_FI_prefixes'] = 3
            rows.append(row)
assert hashlib.sha256(exe.read_bytes()).hexdigest() == digest
print(json.dumps(dict(image_sha256=digest, rows=rows, mode=mode, fresh_public_server=False)))
