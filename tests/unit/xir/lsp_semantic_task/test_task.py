from concurrent.futures import ThreadPoolExecutor, as_completed
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
assert mode in ('--normal', '--resources', '--counterfactual')
digest = hashlib.sha256(exe.read_bytes()).hexdigest()
env = {**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())}
maximum = str((1 << 64) - 1)
stats_re = re.compile(rb'SEM method=(\d+) scenario=(\d+) source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) physical0/0 JSON0/0 app0/0')


def unpack(data):
    values = []
    while data:
        header, separator, rest = data.partition(b'\r\n\r\n')
        assert separator and re.fullmatch(rb'Content-Length: [0-9]+', header), data[:120]
        size = int(header.split(b': ')[1])
        assert 0 < size <= len(rest)
        values.append(json.loads(rest[:size].decode('utf-8')))
        data = rest[size:]
    return values


def invoke(uri, method, *args):
    return subprocess.run([str(exe.resolve()), uri, '--semantic', str(method), *map(str, args)],
                          input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                          timeout=290 if mode == '--resources' else 20)


def baseline(result):
    value = re.search(rb'^BASELINE (\{.*\})\r?$', result.stderr, re.M)
    assert value, result.stderr[-3000:]
    value = json.loads(value[1])
    assert value['resultId'] == '1' and value['data'] and len(value['data']) % 5 == 0
    return value


def probe(uri, method):
    result = invoke(uri, method, '--resource', maximum, maximum)
    assert result.returncode == 0, (method, result.returncode, result.stderr[-3000:])
    match = stats_re.search(result.stderr)
    assert match and int(match[1]) == method and int(match[2]) == 0
    assert int(match[3]) > 0 and int(match[4]) > 0 and all(int(match[n]) == 0 for n in (5, 6, 7))
    assert all(int(match[n]) > 0 for n in (8, 9, 10))
    return dict(method=method, source=int(match[3]), json=int(match[4]),
                allocated=int(match[8]), peak=int(match[9]), work=int(match[10]), full_FI=False)


def check_value(value, original, method, scenario, changed=None):
    if method == 13:
        assert 'resultId' not in value and set(value) == {'data'}, value
        expected = []
        row, column = 0, 0
        stream = changed['data'] if changed else original['data']
        for offset in range(0, len(stream), 5):
            line, col, length, kind, flags = stream[offset:offset + 5]
            row += line
            column = column + col if line == 0 else col
            assert length > 0 and 0 <= kind < 22 and 0 <= flags < 512
            if row == 0:
                expected += stream[offset:offset + 5]
        assert value['data'] == expected and expected, value
    elif method == 12 and scenario == 21:
        assert set(value) == {'resultId', 'edits'} and value['resultId'] == '2' and value['edits'], value
        restored = list(original['data'])
        for edit in value['edits']:
            start, count = edit['start'], edit['deleteCount']
            assert isinstance(start, int) and isinstance(count, int) and 0 <= start <= len(restored) and 0 <= count <= len(restored) - start
            restored[start:start + count] = edit.get('data', [])
        assert restored == changed['data'], (value, restored, changed)
    elif method == 12 and scenario != 13:
        assert value == {'resultId': '2', 'edits': []}, value
    else:
        assert value == {'resultId': '2', 'data': changed['data'] if changed else original['data']}, value


rows = []
with tempfile.TemporaryDirectory(prefix='semantic-task-😀-#-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="semantic-task"\nmain="root.xr"\n', encoding='utf-8')
    entry = root / 'root.xr'
    entry.write_text('invalid disk text is replaced by the captured editor bytes', encoding='utf-8')
    uri = entry.as_uri()
    measured = [] if mode == '--counterfactual' else [probe(uri, method) for method in range(11, 14)]
    if mode == '--normal':
        for measurement in measured:
            method = measurement['method']
            scenarios = [*range(11), *range(12, 19), 21] + ([19] if method == 13 else [])
            for scenario in scenarios:
                extra = [measurement['json'] - 1] if scenario == 18 else []
                result = invoke(uri, method, scenario, *extra)
                assert result.returncode == 0, (method, scenario, result.returncode, result.stderr[-3500:])
                original = baseline(result)
                changed = re.search(rb'^CHANGED (\{.*\})\r?$', result.stderr, re.M)
                changed = json.loads(changed[1]) if changed else None
                assert (changed is not None) == (scenario == 21)
                if scenario == 15:
                    value = json.loads(result.stdout)
                    check_value(value, original, method, scenario)
                    assert f'SEM_DEATH method={method} original_server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0'.encode() in result.stderr
                    rows.append(dict(method=method, scenario=scenario, producer_death=True, one_shot=True))
                    continue
                match = stats_re.search(result.stderr)
                assert match and int(match[1]) == method and int(match[2]) == scenario, result.stderr[-2000:]
                messages = unpack(result.stdout)
                rejected = scenario == 12 or (scenario == 10 and method != 13)
                if scenario in (5, 6, 9, 18) or rejected:
                    assert messages == [], (method, scenario, messages)
                else:
                    assert len(messages) == (2 if scenario == 17 else 1), (method, scenario, messages)
                    first = messages[0]
                    assert first['id'] == 'source\0λ' and first['jsonrpc'] == '2.0'
                    if scenario in (1, 2, 16):
                        assert first['error']['code'] == -32800, first
                    elif scenario in (3, 4) or (scenario == 8 and method != 13):
                        assert first['error']['code'] == -32803, first
                    elif scenario == 19:
                        assert first['error']['code'] == -32603, first
                    else:
                        assert 'error' not in first, (method, scenario, first)
                        check_value(first['result'], original, method, scenario, changed)
                    if scenario == 17:
                        second = messages[1]
                        assert second['id'] == 42 and second['jsonrpc'] == '2.0'
                        if method == 13:
                            check_value(second['result'], original, 11, scenario)
                        else:
                            assert second['error']['code'] == -32803 and 'cache changed' in second['error']['message'], second
                assert int(match[7]) == (1 if scenario in (5, 6, 18) else 0), result.stderr[-2000:]
                row = dict(method=method, scenario=scenario, wire_messages=len(messages),
                           cache_and_three_domains_checked=True)
                if scenario == 2:
                    interior = re.search(rb'INTERIOR mode=(\d+) trigger=(\d+) calls=(\d+)', result.stderr)
                    assert interior and int(interior[1]) == method and int(interior[2]) <= int(interior[3]) <= int(interior[2]) + 64
                    row.update(cancel_trigger=int(interior[2]), work_calls=int(interior[3]))
                if scenario == 14:
                    checks = re.search(rb'SEM_ADMISSION method=(\d+) checks=(\d+)', result.stderr)
                    assert checks and int(checks[1]) == method and int(checks[2]) >= 5
                    row['admission_checks'] = int(checks[2])
                if scenario == 18:
                    assert int(match[6]) == 1
                    row['serialization_FI_prefix'] = measurement['json'] - 1
                rows.append(row)
    elif mode == '--counterfactual':
        for method in range(11, 14):
            result = invoke(uri, method, 20)
            marker = f'BEFORE actual SourceTask rejected semantic mode={method} status=1 physical0/0 JSON0/0 app0/0'
            assert result.returncode == 1 and not result.stdout and result.stderr.count(marker.encode()) == 1, (method, result.returncode, result.stderr[-3000:])
            rows.append(dict(method=method, old_actual_task_rejected=True))
    else:
        for measurement in measured:
            method = measurement['method']
            result = invoke(uri, method, '--axes', measurement['allocated'], measurement['peak'], measurement['work'])
            assert result.returncode == 0, (method, 'axes', result.returncode, result.stderr[-3500:])
            marker = re.search(rb'SEM_AXES method=(\d+) checked=9 budget=(\d+) physical0/0 JSON0/0 app0/0', result.stderr)
            assert marker and int(marker[1]) == method
            messages = unpack(result.stdout)
            assert sum('result' in message for message in messages) == 6, messages
            for message in messages:
                if 'error' in message:
                    assert message['error']['code'] == -32603
                    assert re.search(rf'status={int(marker[2])}\)$', message['error']['message']), message
            rows.append(dict(method=method, resource_axes_checked=9, successful_boundaries=6))
        jobs = []
        for measurement in measured:
            for domain in ('source', 'json'):
                total = measurement[domain]
                for start in range(0, total, 64):
                    jobs.append((measurement['method'], domain, start, min(64, total - start), total))

        def chunk(job):
            method, domain, start, count, total = job
            result = invoke(uri, method, '--chunk', domain, start, count, total)
            assert result.returncode == 0, (job, result.returncode, result.stderr[-3500:])
            marker = f'SEM_CHUNK method={method} domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0'
            assert result.stderr.count(marker.encode()) == 1, (job, result.stderr[-2500:])
            for message in unpack(result.stdout):
                assert message['id'] == 'source\0λ' and message['error']['code'] == -32603, (job, message)
            return dict(method=method, domain=domain, start=start, count=count, total=total)

        with ThreadPoolExecutor(max_workers=8) as pool:
            for future in as_completed([pool.submit(chunk, job) for job in jobs]):
                rows.append(future.result())
        assert sum(row.get('count', 0) for row in rows) == sum(row['source'] + row['json'] for row in measured)
        assert len([row for row in rows if 'resource_axes_checked' in row]) == 3
assert hashlib.sha256(exe.read_bytes()).hexdigest() == digest
print(json.dumps(dict(mode=mode, image_sha256=digest, measured=measured, rows=rows,
                      full_FI=mode == '--resources', fresh_public_server=False)))
