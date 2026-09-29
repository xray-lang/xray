"""Assert independent cleanup escape diagnostics and process exit status."""
import subprocess
import sys

hint = "  a defer body must not let errors escape (spec 8.3.1); this is not catchable\n"
for mode in [5, 6, 8, 9, 11, 24, 25]:
    p = subprocess.run([sys.argv[1], str(mode)], capture_output=True, text=True, timeout=30)
    escaped = "E0444: defer cleanup cannot suspend or create tasks" if mode == 6 else "<no message>" if mode == 11 else "E0420: division by zero"
    expected = "E0443: error escaped a defer body: " + escaped + "\n"
    if mode in [9, 24]:
        expected += "  in-flight error at the time of cleanup: <no message>\n"
    if mode == 25:
        expected += "  in-flight error at the time of cleanup: E0420: division by zero\n"
    expected += hint
    assert p.returncode == 70 and p.stdout == "" and p.stderr == expected, (mode, p.returncode, p.stdout, p.stderr, expected)
print("Seven cleanup escape subprocesses matched independent exit status and exact bytes")
