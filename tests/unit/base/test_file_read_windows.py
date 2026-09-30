"""Run physical-file tests inside a private directory with real NTFS junctions."""
from pathlib import Path
import ctypes
from ctypes import wintypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def junction(link: Path, target: Path) -> None:
    link.mkdir()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p,
                                      wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                      ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    kernel.DeviceIoControl.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(str(link), 0x40000000, 0, None, 3, 0x02200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        substitute = ("\\??\\" + str(target)).encode("utf-16-le")
        printable = str(target).encode("utf-16-le")
        paths = substitute + b"\0\0" + printable + b"\0\0"
        body = struct.pack("<HHHH", 0, len(substitute), len(substitute) + 2, len(printable)) + paths
        record = ctypes.create_string_buffer(struct.pack("<IHH", 0xA0000003, len(body), 0) + body)
        returned = wintypes.DWORD()
        if not kernel.DeviceIoControl(handle, 0x000900A4, record, len(record) - 1,
                                      None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        if not kernel.CloseHandle(handle):
            raise ctypes.WinError(ctypes.get_last_error())


def main() -> int:
    workspace = Path.cwd().resolve()
    scope = Path(tempfile.mkdtemp(prefix="xray file read ", dir=workspace)).resolve()
    assert scope.parent == workspace and scope.name.startswith("xray file read ")
    links = [scope / "root" / "escape", scope / "root" / "linked", scope / "alias"]
    try:
        root = scope / "root"
        (root / "inside").mkdir(parents=True)
        (scope / "outside").mkdir()
        (root / "中文根" / "inside").mkdir(parents=True)
        for path in [root / "inside" / "data", scope / "outside" / "data",
                     root / "中文", root / "中文根" / "inside" / "data"]:
            path.write_bytes(b"abc")
        (root / "empty").write_bytes(b"")
        (root / "binary").write_bytes(b"a\0b")
        (root / "large").write_bytes(b"Q" * 140000)
        junction(links[0], scope / "outside")
        junction(links[1], root / "inside")
        junction(links[2], root)
        return subprocess.run([sys.argv[1], str(root), str(links[2])], check=False).returncode
    finally:
        for link in reversed(links):
            assert link.parent.resolve().is_relative_to(scope)
            if os.path.lexists(link):
                os.rmdir(link)
        assert scope.parent == workspace and scope.name.startswith("xray file read ")
        shutil.rmtree(scope)


if __name__ == "__main__":
    raise SystemExit(main())
