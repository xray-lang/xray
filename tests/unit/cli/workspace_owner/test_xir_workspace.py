from pathlib import Path
import os, subprocess, sys, tempfile

parent = Path(tempfile.mkdtemp(prefix='workspace-fixture-', dir=Path(sys.argv[1]).resolve().parent))
result = subprocess.run([sys.argv[1], str(parent)], timeout=220, capture_output=True)
sys.stdout.buffer.write(result.stdout)
sys.stderr.buffer.write(result.stderr)
if result.returncode:
    print('retained failure directory:', parent, flush=True)
    raise SystemExit(result.returncode)
assert list(parent.iterdir()) == [], list(parent.iterdir())
process = subprocess.Popen([sys.argv[1], str(parent), '--compiler'], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
paths = {}
while len(paths) < 3:
    line = process.stdout.readline()
    if not line:
        raise RuntimeError(process.stderr.read())
    print(line, end='')
    for name in ('root', 'input', 'output'):
        if line.startswith('compiler_' + name + '='):
            paths[name] = Path(line.strip().split('=', 1)[1])
environment = dict(os.environ, TEMP=str(paths['output']), TMP=str(paths['output']))
for name in ('CL', '_CL_', 'LINK', '_LINK_'):
    environment.pop(name, None)
for source, output, expected in [('source.c', 'compiler-unrecorded.tmp', 0), ('bad.c', 'failed.obj', 1)]:
    command = [sys.argv[2], '/nologo', '/std:c11', '/MD', '/O2', '/W4', '/WX', '/c',
               '/Fo' + str(paths['output'] / output), str(paths['input'] / source)]
    compiled = subprocess.run(command, cwd=paths['input'], env=environment, capture_output=True, timeout=30)
    print('real MSVC command:', command, 'exit:', compiled.returncode)
    sys.stdout.flush()
    sys.stdout.buffer.write(compiled.stdout + compiled.stderr)
    assert (compiled.returncode != 0) == bool(expected)
    if expected:
        assert b'workspace-compiler-failure' in compiled.stdout + compiled.stderr
    else:
        assert (paths['output'] / output).read_bytes()[:2] == b'\x64\x86'
stdout, stderr = process.communicate('continue\n', timeout=30)
print(stdout, end=''); print(stderr, end='', file=sys.stderr)
assert process.returncode == 0
assert list(parent.iterdir()) == [], list(parent.iterdir())
parent.rmdir()
print('independent disk oracle: ordinary and real compiler private outputs removed after BUDGET')
