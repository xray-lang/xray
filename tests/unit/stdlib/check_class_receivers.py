"""Verify the real parser's class receiver contracts for every ordinary module."""
from pathlib import Path
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    modules = sorted(path for path in (root / "stdlib").glob("*/*.xr")
                     if path.name == path.parent.name + ".xr")
    if len(modules) < 31:
        raise SystemExit("ordinary module coverage unexpectedly shrank")
    binary = str(args.binary.resolve())
    subprocess.run([binary, "--self-test"], check=True, timeout=60)
    subprocess.run([binary, "--verify", *map(str, modules)], check=True, timeout=60)
    print(f"All {len(modules)} complete ordinary module ASTs use valid class receivers")


if __name__ == "__main__":
    main()
