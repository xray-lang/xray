"""Require the helper executable to cover every original non-server test."""
from pathlib import Path
import re
import subprocess
import sys
from generate_subset import function_span

original = Path(sys.argv[2]).read_text(encoding='utf-8')
wrapper = Path(__file__).with_name('test_original.c').read_text(encoding='utf-8')
declared = re.findall(r'^TEST\((\w+)\)', original, re.MULTILINE)
registered = re.findall(r'^\s+RUN_TEST\((\w+)\);', original, re.MULTILINE)
covered = re.findall(r'^\s+RUN_TEST\((\w+)\);', wrapper, re.MULTILINE)
generated = Path(sys.argv[3]).read_text(encoding='utf-8')
if len(declared) != 16 or registered != declared:
    raise SystemExit('Original config test inventory changed; reconcile every case.')
if covered != [name for name in declared if name != 'config_defaults']:
    raise SystemExit('Helper runner must cover every original non-server case in order.')
for match in re.finditer(r'^TEST\((\w+)\)', original, re.MULTILINE):
    if match[1] != 'config_defaults':
        start, end = function_span(original, match.start())
        if original[start:end] not in generated:
            raise SystemExit('Original test body changed: ' + match[1])
print('Original inventory: 16; independent helpers: 15; real server defaults: NOT_RUN', flush=True)
result = subprocess.run([sys.argv[1]], timeout=29)
raise SystemExit(result.returncode)
