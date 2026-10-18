"""Execute a complete governed math family with disposable, removable sources."""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

code = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(code / 'tests/cli'))
from xir_no_child_process import NoChildProcess

parser = argparse.ArgumentParser()
parser.add_argument('binary')
parser.add_argument('--emit')
parser.add_argument('--direct', action='store_true')
args = parser.parse_args()
governed = (code / 'stdlib/math/math.xr').read_text(encoding='utf-8')
consumer = Path(__file__).with_name('root.xr').read_text(encoding='utf-8')
for kind, pattern in [('functions', r'export fn (\w+)'), ('constants', r'export const (\w+)')]:
    names = set(re.findall(pattern, governed))
    used = set(re.findall(r'math\.(\w+)', consumer))
    if names - used:
        raise SystemExit('incomplete public math ' + kind + ': ' + str(sorted(names - used)))
with tempfile.TemporaryDirectory(prefix='xir-complete-math-') as temporary:
    root = Path(temporary)
    if not args.direct:
        (root / 'math').mkdir()
        shutil.copyfile(code / 'stdlib/math/math.xr', root / 'math/math.xr')
        for name in ('root.xr', 'ctfe.xr'):
            shutil.copyfile(Path(__file__).with_name(name), root / name)
    command = [args.binary] + ([] if args.direct else [str(root)])
    if args.emit:
        command.append(args.emit)
    policy = NoChildProcess(code, root)
    result = policy.run('complete-math-source-free', command, dict(os.environ))
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    if policy.supported:
        print('NoChild absolute-path denial control and inherited Job process limit2 PASS', flush=True)
    raise SystemExit(result.returncode)
