"""Run the real scalar preparation and binding pipelines under their original limits."""
from pathlib import Path
import subprocess
import sys
import tempfile

executable, source = map(Path, sys.argv[1:3])
mode = sys.argv[3]
assert mode in ('measure','faults','profile','binding-measure')
with tempfile.TemporaryDirectory(prefix='xir-scalar-resources-') as temporary:
    root=Path(temporary).resolve()
    entry=root/'root.xr'
    entry.write_bytes((source/'tests/fixtures/xir_scalar_methods/resources.xr').read_bytes())
    subprocess.run([str(executable.resolve()),str(root),str(entry),mode],check=True)
