"""Qualify the complete governed string method closure under original caps."""
from pathlib import Path
import subprocess
import sys
import tempfile
executable,source=map(Path,sys.argv[1:3]);mode=sys.argv[3]
assert mode in ('measure','faults','profile','binding-measure')
with tempfile.TemporaryDirectory(prefix='xir-string-owned-') as temporary:
    root=Path(temporary).resolve();entry=root/'root.xr'
    entry.write_bytes((source/'tests/fixtures/xir_string_owned/resources.xr').read_bytes())
    subprocess.run([str(executable.resolve()),str(root),str(entry),mode],check=True)
