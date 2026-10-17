"""Keep source-input rejection silent outside its actual diagnostic owner."""
import subprocess
import sys

result = subprocess.run([sys.argv[1]], capture_output=True, timeout=110)
expected = b"".join(
    f"entry={index} status=3 location=2:1 message=source text contains an embedded NUL\n".encode()
    for index in range(14)
)
actual = result.stdout.replace(b"\r\n", b"\n")
if result.returncode or result.stderr or actual != expected:
    sys.stdout.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(f"source input diagnostics: exit={result.returncode}, exact stdout={actual == expected}")
print("14 public input paths: exact NUL diagnostics, ownership, retry and silent stderr PASS")
