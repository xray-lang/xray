from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile

exe, stdlib = map(Path, sys.argv[1:3])
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
    for mode in range(12):
        result = subprocess.run([str(exe.resolve()), entry.as_uri(), library.as_uri(), str(mode)],
                                input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=20)
        assert result.returncode == 0, (mode, result.returncode, result.stderr[-2500:])
        messages = unpack(result.stdout)
        if mode in (6, 7, 9, 10):
            assert messages == [], (mode, messages)
        else:
            assert len(messages) == 1 and messages[0]['id'] == 'source\0λ' and messages[0]['jsonrpc'] == '2.0', messages
            message = messages[0]
            if mode in (4, 11):
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
print(json.dumps({'cases': rows, 'actual_Source_request_run': True, 'editor_table_destroyed_before_run': True,
                  'compiler_and_JSON_live_zero': True, 'full_FI': False}))
