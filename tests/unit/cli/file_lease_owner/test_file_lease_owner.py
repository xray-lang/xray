"""Independent file bytes and SHA for role-neutral native input leases."""
from pathlib import Path
import hashlib,subprocess,sys,tempfile
data=b'same handle\0'+bytes.fromhex('e4b8ade69687')+b'\r\n'
with tempfile.TemporaryDirectory(prefix='xray-file-lease-') as temporary:
    root=Path(temporary);(root/'input.bin').write_bytes(data);(root/'empty.bin').write_bytes(b'')
    (root/'中文.bin').write_bytes(data)
    (root/'child').mkdir();(root/'child'/'input.bin').write_bytes(data)
    result=subprocess.run([sys.argv[1],str(root)],capture_output=True,timeout=170)
    sys.stdout.buffer.write(result.stdout);sys.stderr.buffer.write(result.stderr)
    assert result.returncode==0,result.returncode
    records=dict(line.split(' ',1) for line in result.stdout.decode('utf-8').splitlines() if line.startswith(('SHA ','BYTES ')))
    assert records['SHA']==hashlib.sha256(data).hexdigest()
    assert records['BYTES']==str(len(data))
print('independent length, SHA and repeated same-handle reads PASS')
