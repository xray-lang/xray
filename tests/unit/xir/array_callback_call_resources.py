"""Keep every Array callback consumer in one original-ledger preparation."""
from pathlib import Path
import subprocess
import sys
import tempfile

executable, source = map(Path, sys.argv[1:3])
mode = sys.argv[3]
assert mode in ('measure', 'faults', 'binding-measure')
sys.path.insert(0, str(source/'tests/cli'))
from xir_array_callback_call_cases import CASES

with tempfile.TemporaryDirectory(prefix='xir-array-callback-calls-') as temporary:
    root = Path(temporary).resolve()
    bodies = []
    for name, module, body, _ in CASES:
        (root/(name+'-values.xr')).write_text(module, encoding='utf-8')
        bodies.append(body)
    entry = root/'root.xr'
    entry.write_text('\n'.join(bodies), encoding='utf-8')
    subprocess.run([str(executable.resolve()), str(root), str(entry), mode], check=True)
