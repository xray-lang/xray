"""A panic escaping Source defer remains fatal when the host drops suspension."""
import subprocess
import sys

for executable in sys.argv[1:]:
    result = subprocess.run([executable, "--fatal-drop"], capture_output=True)
    assert result.returncode not in (0, 99), (executable, result.returncode, result.stderr)
    assert b"E0443: error escaped a defer body: E0445: cleanup" in result.stderr, result.stderr
    assert b"incorrectly returned to the host" not in result.stderr, result.stderr
print("Source VM/native fatal cleanup retained E0443 and never returned to the host")
