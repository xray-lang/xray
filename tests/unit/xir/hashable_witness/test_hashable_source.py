"""Real Source semantic contracts, with syntax checked independently."""
import argparse
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    fixtures = pathlib.Path(__file__).with_name("fixtures")
    for path in sorted(fixtures.glob("*.xr")):
        syntax = subprocess.run([args.binary, "check", "-S", str(path)], capture_output=True, text=True, encoding="utf-8")
        assert syntax.returncode == 0, (path.name, "syntax", syntax.stdout, syntax.stderr)
        checked = subprocess.run([args.binary, "check", str(path)], capture_output=True, text=True, encoding="utf-8")
        accepted = not path.name.startswith("reject_")
        assert (checked.returncode == 0) == accepted, (path.name, checked.stdout, checked.stderr)
        if not accepted:
            assert "constraint name does not resolve to an interface" not in checked.stdout + checked.stderr, (path.name, checked.stdout, checked.stderr)
        print(path.name, "PASS" if accepted else "REJECTED")


if __name__ == "__main__":
    main()
