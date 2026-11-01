"""Exercise physical-root file input with real POSIX links and special files."""
from pathlib import Path
import os
import socket
import subprocess
import sys
import tempfile


def main() -> int:
    # Keep UNIX socket paths below the smaller macOS sockaddr_un limit.
    with tempfile.TemporaryDirectory(prefix="xray-file-read-", dir="/tmp") as temporary:
        scope = Path(temporary).resolve()
        root = scope / "root 中文"
        inside = root / "inside"
        inside.mkdir(parents=True)
        outside = scope / "outside"
        outside.mkdir()
        sibling = scope / "root 中文-extra"
        sibling.mkdir()
        for path in (inside / "data", outside / "data", sibling / "data", root / "中文"):
            path.write_bytes(b"abc")
        (root / "empty").write_bytes(b"")
        (root / "binary").write_bytes(b"a\0b")
        (root / "large").write_bytes(b"Q" * 140000)
        (root / "escape").symlink_to(outside, target_is_directory=True)
        (root / "linked").symlink_to("inside", target_is_directory=True)
        (root / "file-link").symlink_to("inside/data")
        (root / "file-absolute").symlink_to(inside / "data")
        (root / "escape-file").symlink_to(outside / "data")
        (root / "escape-sibling").symlink_to(sibling / "data")
        (root / "dangling-in").symlink_to("absent/child")
        (root / "dangling-out").symlink_to(outside / "missing")
        (root / "dangling-out-deep").symlink_to(outside / "absent" / "child")
        (root / "parent-link").symlink_to("inside/../inside/data")
        (root / "directory-link").symlink_to("inside/")
        (root / "trailing-file-link").symlink_to("inside/data/")
        (root / "loop").symlink_to("loop")
        (outside / "return").symlink_to(inside, target_is_directory=True)
        (scope / "alias").symlink_to(root, target_is_directory=True)
        for index in range(40):
            target = f"chain-{index + 1}" if index < 39 else "inside/data"
            (root / f"chain-{index}").symlink_to(target)
        (root / "too-many").symlink_to("chain-0")
        deep = root
        for _ in range(258):
            deep /= "d"
            deep.mkdir()
        (deep / "data").write_bytes(b"abc")
        os.mkfifo(root / "fifo")
        with socket.socket(socket.AF_UNIX) as endpoint:
            endpoint.bind(str(root / "socket"))
            completed = subprocess.run(
                [sys.argv[1], str(root), str(scope / "alias")],
                check=False, timeout=25,
            )
        return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
