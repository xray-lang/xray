"""Verify Source VM cleanup barriers against fixed diagnostics."""
import subprocess
import sys

for executable in sys.argv[1:]:
    for mode in range(4):
        result = subprocess.run([executable, "--fatal", str(mode)],
                                capture_output=True, timeout=30)
        expected = "E0443: error escaped a defer body: E0420: division by zero\n"
        if mode == 1:
            expected += "  in-flight error at the time of cleanup: <no message>\n"
        if mode == 2:
            expected += "  in-flight error at the time of cleanup: E0420: division by zero\n"
        expected += "  a defer body must not let errors escape (spec 8.3.1); this is not catchable\n"
        stderr = result.stderr.replace(b"\r\n", b"\n")
        assert (result.returncode, result.stdout, stderr) == (70, b"", expected.encode()), (
            executable, mode, result.returncode, result.stdout, result.stderr)
print("Source VM cleanup fatal exits matched independent status and diagnostics")
