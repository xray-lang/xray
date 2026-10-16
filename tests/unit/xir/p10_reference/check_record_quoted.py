"""Verify parser location and Source ownership rejection for a quoted record key."""

import subprocess
import sys


def main():
    result = subprocess.run(
        [sys.argv[1], "--reject-structure", "record_reject_quoted", "0",
         "module graph build failed"], capture_output=True, text=True, encoding="utf-8",
        errors="strict", check=False, timeout=25)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    assert result.returncode == 0, result.returncode
    assert "record keys must be identifiers; use Map for string keys" in result.stderr
    assert "record_reject_quoted.xr:2:21" in result.stderr
    assert "independent expectation passed; compiler ledger restored" in result.stdout


if __name__ == "__main__":
    main()
