"""Compare generated source bytes against their actual canonical file inputs."""
import hashlib
import pathlib
import re
import subprocess
import sys

result = subprocess.run([sys.argv[1]], check=True, capture_output=True, text=True, encoding='utf8')
print(result.stdout, end='')
root = pathlib.Path(sys.argv[2])
records = re.findall(r'^SOURCE (\w+) (\d+) ([0-9a-f]{64})$', result.stdout, re.M)
assert len(records) == 2
assert {name for name, _, _ in records} == {'io', 'path'}
for name, length, digest in records:
    data = (root / 'stdlib' / name / (name + '.xr')).read_bytes()
    assert int(length) == len(data)
    assert digest == hashlib.sha256(data).hexdigest()
