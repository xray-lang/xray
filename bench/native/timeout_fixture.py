"""Create descendants that retain standard streams until the runner stops them."""

from pathlib import Path
import os
import subprocess
import sys
import time


sys.stdout.reconfigure(newline="\n")
sys.stderr.reconfigure(newline="\n")
mode, output = sys.argv[1], Path(sys.argv[2])
if mode in ("child", "late-child", "leaf"):
    name = "leaf" if mode == "leaf" else "child"
    (output / f"{name}.pid").write_text(str(os.getpid()), encoding="ascii")
    if mode == "late-child":
        while not (output / "spawn").exists():
            time.sleep(0.01)
    if mode != "leaf":
        subprocess.Popen([sys.executable, __file__, "leaf", str(output)],
                         stdout=sys.stdout, stderr=sys.stderr)
    time.sleep(60)
else:
    child_mode = "late-child" if mode == "late" else "child"
    subprocess.Popen([sys.executable, __file__, child_mode, str(output)],
                     stdout=sys.stdout, stderr=sys.stderr)
    deadline = time.monotonic() + 5
    names = ("child",) if mode == "late" else ("child", "leaf")
    while not ((output / "observed").exists() and all((output / f"{name}.pid").exists() and
                  (output / f"{name}.pid").read_text(encoding="ascii").isdigit()
                  for name in names)):
        if time.monotonic() >= deadline:
            raise RuntimeError("descendants did not start")
        time.sleep(0.01)
    print("parent", flush=True)
    print("retained stderr", file=sys.stderr, flush=True)
    if mode == "timeout":
        time.sleep(60)
