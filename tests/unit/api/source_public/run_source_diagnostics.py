"""Require actual public parser failures to avoid unsolicited process output."""
import subprocess
import sys

result = subprocess.run([sys.argv[1]], capture_output=True, timeout=110)
expected = b"".join(
    f"entry={index} status=3 location=2:1 message=expected expression\n".encode()
    for index in range(9)
)
# The C runtime uses platform text-mode line endings for the test's own stdout.
actual = result.stdout.replace(b"\r\n", b"\n")
if result.returncode or result.stderr or actual != expected:
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(f"public diagnostics: exit={result.returncode}, exact stdout={actual == expected}")
print("9 public entries: exact owned parser diagnostics, retry and silent stderr PASS")
