"""Give the complete Source fault matrix independent inputs and its CTest deadline."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[4]
with tempfile.TemporaryDirectory(prefix='xir-math-source-faults-') as temporary:
    work = Path(temporary)
    (work / 'math').mkdir()
    shutil.copyfile(root / 'stdlib/math/math.xr', work / 'math/math.xr')
    raise SystemExit(subprocess.call([sys.argv[1], str(work)]))
