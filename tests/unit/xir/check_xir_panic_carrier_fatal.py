from pathlib import Path
import subprocess
import sys
binary = Path(sys.argv[1])
hint = b'  a defer body must not let errors escape (spec 8.3.1); this is not catchable\n'
for mode, message in [('fatal', b'a\0\xe4\xb8\xadz\0'), ('fatal-lines', b'l\nr\r\0t\t'), ('fatal-empty', b''), ('fatal-driver', b'a\0\xe4\xb8\xadz\0')]:
    result = subprocess.run([str(binary), mode], capture_output=True, timeout=30)
    expected = b'E0443: error escaped a defer body: E0445: ' + message + b'\n'
    expected += b'  in-flight error at the time of cleanup: E0445: ' + message + b'\n' + hint
    assert result.returncode == 70 and not result.stdout and result.stderr == expected, (mode, result.returncode, result.stdout, result.stderr, expected)
    print(f'{mode}: exit70, zero stdout and {len(expected)} exact stderr bytes PASS')
