"""Check object-target composition separately from actual native compilation."""
from pathlib import Path
import subprocess,sys,tempfile

with tempfile.TemporaryDirectory(prefix='xray-make-target-') as temporary:
    report=Path(temporary)/'actual.d'
    report.write_bytes(b'C:\\objects\\right\\ name.obj: C:\\source\\input.c \\\r\n C:\\include\\header.h\r\n')
    result=subprocess.run([sys.argv[1],'--make-target',str(report),'c:/objects/right name.obj'],capture_output=True,timeout=30)
    sys.stdout.buffer.write(result.stdout);sys.stderr.buffer.write(result.stderr)
    assert result.returncode==0,result.returncode
