"""Create descendants that retain standard streams until the runner stops them."""

from pathlib import Path
import os
import subprocess
import sys
import time


sys.stdout.reconfigure(newline="\n")
sys.stderr.reconfigure(newline="\n")
mode, output = sys.argv[1], Path(sys.argv[2])
if mode in ("child", "leaf"):
    (output / f"{mode}.pid").write_text(str(os.getpid()), encoding="ascii")
    if mode == "child":
        subprocess.Popen([sys.executable, __file__, "leaf", str(output)],
                         stdout=sys.stdout, stderr=sys.stderr)
    time.sleep(60)
else:
    subprocess.Popen([sys.executable, __file__, "child", str(output)],
                     stdout=sys.stdout, stderr=sys.stderr)
    deadline = time.monotonic() + 5
    while not ((output / "observed").exists() and all((output / f"{name}.pid").exists() and
                  (output / f"{name}.pid").read_text(encoding="ascii").isdigit()
                  for name in ("child", "leaf"))):
        if time.monotonic() >= deadline:
            raise RuntimeError("descendants did not start")
        time.sleep(0.01)
    print("parent", flush=True)
    print("retained stderr", file=sys.stderr, flush=True)
    if mode == "timeout":
        time.sleep(60)
