"""Build and run sealed native programs through the actual canonical CLI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

# Strict providers reject an assignment of a local to itself; the automatic
# provider choice must not decide whether a generator defect is visible.
SELF_ASSIGNMENT = re.compile(rb"\b(v[0-9]+) = \1;")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xray", type=Path, required=True)
    args = parser.parse_args()
    xray = args.xray.resolve(strict=True)
    root = Path(__file__).resolve().parents[2]
    cases = (
        ("scalar", 'print(42)\n', b"42\n"),
        ("entropy", (root / "demos/07-advanced/crypto_entropy.xr").read_text(encoding="utf-8"), b"6\n"),
    )
    with tempfile.TemporaryDirectory(prefix="xray-native-provider-") as directory:
        folder = Path(directory)
        for name, source, expected in cases:
            path = folder / (name + ".xr")
            output = folder / (name + (".exe" if os.name == "nt" else ""))
            path.write_text(source, encoding="utf-8")
            generated = folder / (name + ".c")
            emitted = subprocess.run([str(xray), "build", str(path), "--native", "--c-only",
                                      "-o", str(generated)], cwd=root, capture_output=True,
                                     timeout=480)
            if emitted.returncode != 0:
                raise RuntimeError(f"{name} C emission exit {emitted.returncode}: " +
                    (emitted.stdout + emitted.stderr).decode("utf-8", errors="replace"))
            self_assigned = SELF_ASSIGNMENT.search(generated.read_bytes())
            if self_assigned:
                raise RuntimeError(f"{name} generated C assigns a local to itself: " +
                                   self_assigned.group(0).decode("ascii"))
            built = subprocess.run([str(xray), "build", str(path), "-o", str(output)],
                                   cwd=root, capture_output=True, timeout=480)
            if built.returncode != 0:
                raise RuntimeError(f"{name} build exit {built.returncode}: " +
                    (built.stdout + built.stderr).decode("utf-8", errors="replace"))
            actual = subprocess.run([str(output)], cwd=folder, capture_output=True, timeout=30)
            if (actual.returncode, actual.stdout, actual.stderr) != (0, expected, b""):
                raise RuntimeError(f"{name} run: exit={actual.returncode} "
                                   f"stdout={actual.stdout!r} stderr={actual.stderr!r}")
            print(json.dumps({"case": name, "exit": actual.returncode,
                              "stdout_hex": actual.stdout.hex(), "stderr_hex": actual.stderr.hex(),
                              "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                              "binary_sha256": hashlib.sha256(output.read_bytes()).hexdigest()}), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
