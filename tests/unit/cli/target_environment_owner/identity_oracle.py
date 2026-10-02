"""Independent command-v2 framing against the real leased-file producer."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
u32 = lambda n: struct.pack('<I', n)
u64 = lambda n: struct.pack('<Q', n)


def string(value):
    data = value.encode('utf-8')
    return u32(len(data)) + data


with tempfile.TemporaryDirectory(prefix='xray-command-v2-') as temporary:
    root = Path(temporary)
    source = root / 'input.c'
    header = root / 'input.h'
    compiler = root / 'compiler.bin'
    for path, data in [(source, b'source'), (header, b'abc'), (compiler, b'compiler')]:
        path.write_bytes(data)
    commands = [
        ('Z:/unopened/编译器.exe', 'Z:/unopened/work', ['display-name', '', '/std:c11'],
         [('=C:', 'C:/frozen'), ('中文', '😄'), ('empty', '')], 3001, 4294967297, 1),
        ('Z:/another.exe', 'Z:/', ['different-argv0'], [], 1, 1, 0),
    ]
    rows = ['xray-target-command-v2', '3', '3', f'7\t{source}', f'4\t{header}',
            f'1\t{compiler}', str(len(commands))]
    for executable, cwd, argv, env, timeout, limit, mode in commands:
        rows += [executable, cwd, str(len(argv)), *argv, str(len(env))]
        for key, value in env:
            rows += [key, value]
        rows += [str(timeout), str(limit), str(mode)]
    manifest = root / 'request.txt'
    manifest.write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
    p = subprocess.run([str(exe), '--diagnostic-lease', str(manifest)], input=b'\n',
                       capture_output=True, timeout=30)
    sys.stdout.buffer.write(p.stdout)
    sys.stderr.buffer.write(p.stderr)
    assert p.returncode == 0
    identities, files = {}, []
    for line in p.stdout.decode('utf-8').splitlines():
        if line.startswith('IDENTITY '):
            _, index, digest = line.split()
            identities[int(index)] = bytes.fromhex(digest)
        elif line.startswith('FILE '):
            _, kind, length, digest, path = line.split(' ', 4)
            data = Path(path).read_bytes()
            assert len(data) == int(length) and hashlib.sha256(data).hexdigest() == digest
            files.append((int(kind), path, int(length), bytes.fromhex(digest)))
    assert len(files) == 3 and [f[1] for f in files] == sorted(f[1] for f in files)
    def file_frame(f):
        kind, path, length, digest = f
        return u32(kind) + string(path) + u64(length) + digest
    for index, domain, policy, predicate in [
        (0, b'xray:xir-provider-files:v1', 3, lambda k: k <= 3),
        (1, b'xray:xir-sysroot-files:v1', 2, lambda k: 4 <= k <= 6),
    ]:
        selected = [f for f in files if predicate(f[0])]
        encoded = domain + u32(policy) + u32(len(selected)) + b''.join(map(file_frame, selected))
        assert hashlib.sha256(encoded).digest() == identities[index]
    encoded = b'xray:xir-target-snapshot:v2' + struct.pack('<4I', 2, 3, 2, 11)
    encoded += string('x86_64-windows-msvc') + identities[0] + identities[1] + u32(len(commands))
    for executable, cwd, argv, env, timeout, limit, mode in commands:
        encoded += string(executable) + string(cwd) + u32(len(argv)) + b''.join(map(string, argv))
        encoded += u32(len(env)) + b''.join(string(k) + string(v) for k, v in env)
        encoded += u32(timeout) + u64(limit) + u32(mode)
    encoded += u32(len(files)) + b''.join(map(file_frame, files))
    assert hashlib.sha256(encoded).digest() == identities[2]
    assert hashlib.sha256(encoded.replace(b'snapshot:v2', b'snapshot:v1', 1)).digest() != identities[2]
    for old in [rows[1:], ['xray-target-command-v1', *rows[1:]]]:
        manifest.write_text('\n'.join(old) + '\n', encoding='utf-8', newline='\n')
        rejected = subprocess.run([str(exe), '--diagnostic-lease', str(manifest)],
                                  capture_output=True, timeout=30)
        assert rejected.returncode == 2 and rejected.stdout == b'STATUS 6\r\n', rejected
print('independent LE framing, unchanged family identities and exact v1 protocol rejection PASS')
