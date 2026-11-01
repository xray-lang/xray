"""Run native discovery and compilation under DOS aliases of an owned temp root."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True)
args = parser.parse_args()
binary = args.binary.resolve()
with tempfile.TemporaryDirectory(prefix='xray-native-alias-') as temporary:
    root = Path(temporary)
    source = root / 'main.xr'
    source.write_bytes(b'print(42)\n')
    buffer = ctypes.create_unicode_buffer(32768)
    count = ctypes.windll.kernel32.GetShortPathNameW(str(root), buffer, len(buffer))
    assert 0 < count < len(buffer), ctypes.get_last_error()
    for label, parent in [('long', str(root)), ('dos-alias', buffer.value)]:
        environment = dict(os.environ, TEMP=parent, TMP=parent)
        probe = subprocess.run([binary, 'toolchain', 'probe', '--json'],
                               env=environment, capture_output=True, timeout=120)
        assert probe.returncode == 0 and not probe.stderr, (label, probe.stdout, probe.stderr)
        facts = json.loads(probe.stdout)
        assert facts['selection']['ready'] and not facts['diagnostics'], facts
        assert all(facts['capabilities'][k] == 'ok' for k in
                   ('cCompile', 'sdkCompile', 'runtimeLink', 'nativeRun')), facts
        output = root / (label + '.exe')
        built = subprocess.run([binary, 'build', source, '-o', output],
                               env=environment, capture_output=True, timeout=120)
        assert built.returncode == 0 and output.is_file(), (label, built.stdout, built.stderr)
        run = subprocess.run([output], capture_output=True, timeout=30)
        assert (run.returncode, run.stdout, run.stderr) == (0, b'42\n', b''), run
        print(label + ': native probe, compile, link and execution PASS')
    print('distinct DOS alias:', buffer.value.casefold() != str(root).casefold())
