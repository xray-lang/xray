"""Compare copied PE bytes against Microsoft's fixed-resource reader, without execution."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import pathlib
import subprocess
import sys

import struct


def fixed_version(path):
    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoSizeExW.restype = wintypes.DWORD
    version.GetFileVersionInfoExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
    version.GetFileVersionInfoExW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    version.VerQueryValueW.restype = wintypes.BOOL
    ignored = wintypes.DWORD()
    size = version.GetFileVersionInfoSizeExW(2, str(path), ctypes.byref(ignored))
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoExW(2, str(path), 0, size, buffer):
        raise ctypes.WinError(ctypes.get_last_error())
    pointer, length = ctypes.c_void_p(), wintypes.UINT()
    if not version.VerQueryValueW(buffer, "\\", ctypes.byref(pointer), ctypes.byref(length)):
        raise ctypes.WinError(ctypes.get_last_error())
    assert pointer.value and length.value == 52
    words = struct.unpack("<13I", ctypes.string_at(pointer, 52))
    assert words[0] == 0xFEEF04BD
    def text(ms, ls):
        return ".".join(map(str, [ms >> 16, ms & 65535, ls >> 16, ls & 65535]))
    return text(words[2], words[3]), text(words[4], words[5]), list(words)


def main():
    executable, compiler, linker, destination = map(pathlib.Path, sys.argv[1:])
    destination.mkdir(parents=True, exist_ok=True)
    facts = []
    for source in (compiler, linker):
        data = source.read_bytes()
        size, digest = len(data), hashlib.sha256(data).hexdigest()
        copy = destination / (source.name + ".bytes")
        copy.write_bytes(data)
        file_version, product_version, words = fixed_version(copy.resolve())
        result = subprocess.run([str(executable), str(copy), file_version, product_version],
                                check=True, capture_output=True, text=True)
        print(result.stdout, end="")
        assert copy.read_bytes() == data
        facts.append(dict(source=str(source), bytes=size, sha256=digest, file_version=file_version,
                          product_version=product_version, fixed_words=words))
    print(json.dumps(facts, indent=2))


if __name__ == "__main__":
    main()
