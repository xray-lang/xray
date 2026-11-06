"""A double fault terminates the embedded execution while the host remains alive."""
import subprocess
import sys

expected = b"host survived cleanup failure: primary445 prior cancellation; owned cleanup after close physical0\n"
for executable in sys.argv[1:]:
    result = subprocess.run([executable, "--fatal-drop"], capture_output=True, timeout=30)
    assert result.returncode == 0, (executable, result.returncode, result.stderr)
    assert result.stdout.replace(b"\r\n", b"\n") == expected, result.stdout
    assert not result.stderr, result.stderr
print("Source VM/native cleanup double fault retained owned panic and cancellation; host survived")
