"""Check typed OOM and the unchanged genuine-malformed-C ICE boundary."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

executable=Path(sys.argv[1]).resolve()
normal=subprocess.run([str(executable)],capture_output=True,text=True)
print(normal.stdout,end='');print(normal.stderr,end='',file=sys.stderr)
assert normal.returncode==0,normal.returncode
with tempfile.TemporaryDirectory(prefix='xray-cgen-ice-') as directory:
    environment=dict(os.environ,XRAY_CGEN_ICE_DIR=directory)
    malformed=subprocess.run([str(executable),'--ice'],env=environment,capture_output=True,text=True)
    assert malformed.returncode!=0,malformed.stdout
    assert '[xi_cgen][ICE]' in malformed.stderr and 'W1_BALANCE' in malformed.stderr,malformed.stderr
    dumps=list(Path(directory).glob('xray_cgen_ice_malformed_test_*.c'))
    assert len(dumps)==1 and dumps[0].read_bytes()==b'void f(void) {\n'
print('genuine malformed-C ICE and complete source dump PASS')
