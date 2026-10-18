from pathlib import Path
import json
import hashlib
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
image_sha256 = hashlib.sha256(exe.read_bytes()).hexdigest()
before = sys.argv[3:] == ['--before']
budget_before = sys.argv[3:] == ['--budget-before']
assert sys.argv[3:] in ([], ['--before'], ['--budget-before'])
env = {**os.environ, 'XRAY_STDLIB_PATH': str(stdlib.resolve())}


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
with tempfile.TemporaryDirectory(prefix='xray-actual-task-', dir=os.getcwd()) as folder:
    root = Path(folder)
    (root / 'xray.toml').write_text('[project]\nname="actual-task"\nversion="1.0.0"\nmain="root.xr"\n', encoding='utf-8')
    entry, library = root / 'root.xr', root / 'lib.xr'
    entry.write_text('this disk text is deliberately invalid', encoding='utf-8')
    library.write_text('this disk text is deliberately invalid', encoding='utf-8')
    for mode in ([0] if budget_before else range(12, 16) if before else range(16)):
        result = subprocess.run([str(exe.resolve()), entry.as_uri(), library.as_uri(), str(mode)],
                                input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=20)
        if budget_before:
            limits = re.search(rb'Task capture limits allocated=(\d+) live=(\d+) work=(\d+)', result.stderr)
            assert result.returncode == 1 and limits and tuple(map(int, limits.groups())) == (1 << 30, 256 << 20, 8 << 30), (result.returncode, result.stderr[-2500:])
            assert b'limits&&limits->allocated_bytes' in result.stderr and not result.stdout, result.stderr
            rows.append({'mode': 0, 'old_CLI_limits': list(map(int, limits.groups())), 'strict_constructor_rejected': True})
            continue
        if before:
            assert result.returncode == 1 and result.stderr.count(b'Actual Source task cancellation did not interrupt') == 1, (mode, result.returncode, result.stderr[-2500:])
        else:
            assert result.returncode == 0, (mode, result.returncode, result.stderr[-2500:])
        messages = unpack(result.stdout)
        if mode in (6, 7, 9, 10):
            assert messages == [], (mode, messages)
        else:
            assert len(messages) == 1 and messages[0]['id'] == 'source\0λ' and messages[0]['jsonrpc'] == '2.0', messages
            message = messages[0]
            if mode in (4, 11) or mode >= 12:
                assert message['error']['code'] == -32800, message
            elif mode == 8:
                assert message['error']['code'] == -32803, message
            else:
                assert 'error' not in message, (mode, message)
                value = message['result']
                if mode in (0, 5):
                    assert value == {'uri': library.as_uri(), 'range': {'start': {'line': 0, 'character': 19}, 'end': {'line': 0, 'character': 24}}}, value
                elif mode == 1:
                    assert len(value) == 1 and value[0]['uri'] == entry.as_uri(), value
                elif mode == 2:
                    assert len(value) == 1 and value[0]['kind'] == 2, value
                else:
                    assert value['contents']['kind'] == 'plaintext' and 'value' in value['contents']['value'] and 'i64' in value['contents']['value'], value
        match = re.search(rb'STATS mode=(\d+) captured=(\d+) compiler_live=(\d+) compiler_bytes=(\d+) json_live=(\d+) json_bytes=(\d+) exit=(\d+) app_live=(\d+) app_bytes=(\d+) admission=(\d+)', result.stderr)
        assert match and all(int(match[i]) == 0 for i in (3, 4, 5, 6, 8, 9)), result.stderr[-1000:]
        assert (int(match[10]) > 14) if mode == 0 else (int(match[10]) == 0), result.stderr
        rows.append({'mode': mode, 'captured_blocks': int(match[2]), 'exit': int(match[7]),
                     'wire_messages': len(messages), 'admission_checks': int(match[10])})
        if mode == 0:
            assert result.stderr.count(b'Actual synchronous Source navigation4 modes strict64Mi8Mi128000000 and output owner death PASS') == 1, result.stderr
            rows[-1]['synchronous_modes'] = 4
        if mode >= 12:
            interior = re.search(rb'INTERIOR mode=(\d+) trigger=(\d+) calls=(\d+) strict64Mi8Mi128000000 physical0/0 JSON0/0 app0/0', result.stderr)
            assert interior and int(interior[1]) == mode, result.stderr
            assert (int(interior[3]) > int(interior[2]) + 64) if before else (int(interior[2]) <= int(interior[3]) <= int(interior[2]) + 64), result.stderr
            rows[-1].update(trigger=int(interior[2]), work_calls=int(interior[3]), interior_cancel=True)
assert hashlib.sha256(exe.read_bytes()).hexdigest() == image_sha256
print(json.dumps({'cases': rows, 'actual_Source_request_run': not budget_before, 'editor_table_destroyed_before_run': not budget_before,
                  'compiler_and_JSON_live_zero': not budget_before, 'counterfactual': before or budget_before,
                  'image_sha256': image_sha256, 'full_FI': False}))
