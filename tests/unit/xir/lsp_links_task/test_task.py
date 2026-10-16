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
env = {**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())}
maximum = str((1 << 64) - 1)
stats_re = re.compile(rb'LINKS source=(\d+) json=(\d+) source_hit=(\d+) json_hit=(\d+) exit=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) physical0/0 JSON0/0 app0/0')


def unpack(data):
    messages = []
    while data:
        header, separator, rest = data.partition(b'\r\n\r\n')
        assert separator and re.fullmatch(rb'Content-Length: [0-9]+', header), data[:100]
        size = int(header.split(b': ')[1])
        assert 0 < size <= len(rest)
        messages.append(json.loads(rest[:size].decode('utf-8')))
        data = rest[size:]
    return messages


def invoke(uri, library, *args):
    return subprocess.run([str(exe.resolve()), uri, '--links', library, *map(str, args)],
                          input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                          timeout=290 if mode == '--resources' else 20)


def check_links(value, library, disk):
    assert value == [
        {'range': {'start': {'line': 0, 'character': 16}, 'end': {'line': 0, 'character': 23}}, 'target': library},
        {'range': {'start': {'line': 1, 'character': 14}, 'end': {'line': 1, 'character': 22}}, 'target': disk}], value


with tempfile.TemporaryDirectory(prefix='links-task-😀-#-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="links-task"\nmain="root.xr"\n', encoding='utf-8')
    for name in ('root', 'lib', 'disk'):
        (root / f'{name}.xr').write_text('invalid disk syntax', encoding='utf-8')
    uri, library, disk = [(root / f'{name}.xr').as_uri() for name in ('root', 'lib', 'disk')]
    rows = []
    if mode == '--counterfactual':
        result = invoke(uri, library, 14)
        assert result.returncode == 1 and result.stdout == b'', result.stderr[-3000:]
        assert b'BEFORE actual SourceTask rejected documentLink mode=14 status=1 physical0/0 JSON0/0 app0/0' in result.stderr
        rows.append({'old_actual_task_rejects_mode14': True})
        measurement = None
    else:
        result = invoke(uri, library, '--resource', maximum, maximum)
        assert result.returncode == 0, result.stderr[-3000:]
        match = stats_re.search(result.stderr)
        assert match and all(int(match[n]) > 0 for n in (1, 2, 6, 7, 8)) and all(int(match[n]) == 0 for n in (3, 4, 5)), result.stderr[-3000:]
        messages = unpack(result.stdout)
        assert len(messages) == 1 and messages[0]['id'] == 'source\0λ'
        check_links(messages[0]['result'], library, disk)
        measurement = dict(source=int(match[1]), json=int(match[2]), allocated=int(match[6]), peak=int(match[7]), work=int(match[8]))
    if mode == '--normal':
        for scenario in [*range(14), 15]:
            result = invoke(uri, library, scenario, *([measurement['json'] - 1] if scenario == 15 else []))
            assert result.returncode == 0, (scenario, result.returncode, result.stderr[-3000:])
            if scenario == 13:
                check_links(json.loads(result.stdout), library, disk)
                assert b'LINKS_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0' in result.stderr
                rows.append(dict(scenario=scenario, producer_death=True, one_shot=True))
                continue
            match = stats_re.search(result.stderr)
            assert match, result.stderr[-3000:]
            messages = unpack(result.stdout)
            if scenario in (5, 6, 9, 15):
                assert not messages, (scenario, messages)
            else:
                assert len(messages) == 1 and messages[0]['id'] == 'source\0λ', (scenario, messages)
                value = messages[0]
                if scenario in (1, 2, 8):
                    assert value['error']['code'] == -32800, value
                elif scenario in (3, 4):
                    assert value['error']['code'] == -32803, value
                elif scenario == 10:
                    assert value['error']['code'] == -32603, value
                else:
                    check_links(value['result'], library, disk)
            assert int(match[5]) == (1 if scenario in (5, 6, 15) else 0), result.stderr[-3000:]
            if scenario == 2:
                interior = re.search(rb'INTERIOR mode=14 trigger=(\d+) calls=(\d+)', result.stderr)
                assert interior and int(interior[1]) <= int(interior[2]) <= int(interior[1]) + 64
            if scenario == 12:
                assert b'LINKS_ADMISSION checks=6' in result.stderr
            if scenario == 15:
                assert int(match[4]) == 1
            rows.append(dict(scenario=scenario, wire_messages=len(messages), three_domains_zero=True))
    if mode == '--resources':
        chunks = [(domain, start, min(64, measurement[domain] - start), measurement[domain])
                  for domain in ('source', 'json') for start in range(0, measurement[domain], 64)]

        def check_chunk(chunk):
            domain, start, count, total = chunk
            result = invoke(uri, library, '--chunk', *chunk)
            assert result.returncode == 0, (chunk, result.returncode, result.stderr[-3000:])
            marker = f'LINKS_CHUNK domain={domain} start={start} count={count} total={total} checked={count} physical0/0 JSON0/0 app0/0'.encode()
            assert marker in result.stderr
            for message in unpack(result.stdout):
                assert message['id'] == 'source\0λ' and message['error']['code'] == -32603 and 'result' not in message, message
            return dict(domain=domain, start=start, count=count)

        with ThreadPoolExecutor(max_workers=8) as pool:
            rows += [future.result() for future in as_completed([pool.submit(check_chunk, chunk) for chunk in chunks])]
        result = invoke(uri, library, '--axes', measurement['allocated'], measurement['peak'], measurement['work'])
        assert result.returncode == 0 and b'LINKS_AXES checked=9 physical0/0 JSON0/0 app0/0' in result.stderr, result.stderr[-3000:]
        messages = unpack(result.stdout)
        assert sum('result' in value for value in messages) == 6, messages
        for value in messages:
            if 'result' in value:
                check_links(value['result'], library, disk)
            else:
                assert value['error']['code'] == -32603, value
        rows.append(dict(resource_axes=9))
    print(json.dumps(dict(image_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), mode=mode,
                          measured=measurement, cases=len(rows), complete_FI=mode == '--resources', rows=rows), ensure_ascii=False))
