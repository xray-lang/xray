"""A discarded complete call must not become a selectable call."""
import subprocess
import sys

result = subprocess.run([sys.argv[1]], capture_output=True, timeout=20)
sys.stdout.buffer.write(result.stdout)
sys.stderr.buffer.write(result.stderr)
assert b"AddressSanitizer" not in result.stderr
assert b"runtime error:" not in result.stderr
assert result.returncode != 0
assert b"status==c->status" in result.stderr
assert b"call syntax mode=9" in result.stdout
assert b"call syntax mode=10" not in result.stdout
print("Unfiltered parser observations rejected by the original recovery fixture")
