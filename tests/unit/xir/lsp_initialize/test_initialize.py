from pathlib import Path
import subprocess
import sys
raise SystemExit(subprocess.run([str(Path(sys.argv[1]).resolve())], timeout=25).returncode)
