import subprocess
import sys

result = subprocess.run([sys.argv[1]], capture_output=True, timeout=25)
assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
assert b"ARGUMENT_REACHABILITY expected=0 actual=1" in result.stderr, result.stderr
assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
print("Unfiltered completed call was rejected by the argument projection regression")
