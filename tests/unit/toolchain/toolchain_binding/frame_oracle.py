"""Independent domain/length encoding; no owner hashing implementation is used."""
import hashlib
import struct
import subprocess
import sys


def sha(data):
    return hashlib.sha256(data).digest()


def text_id(field, value):
    raw = value.encode("utf-8")
    return sha(b"xray:toolchain:" + field + b":v2" + struct.pack("<I", len(raw)) + raw)


run = subprocess.run([sys.argv[1]], check=True, capture_output=True, text=True)
facts = dict(line.split("=", 1) for line in run.stdout.splitlines() if "=" in line)
toolchain = sha(
    b"xray:aot-toolchain:v2" + struct.pack("<II", 2, 3)
    + text_id(b"provider-version", "cl-19.44")
    + text_id(b"target-triple", "windows-x86_64")
    + text_id(b"codegen-options", "opt=2;debug=0")
    + sha(b"sysroot") + sha(b"sdk") + sha(b"target")
)
assert toolchain.hex() == "e2e6c4539d1aa5f29063314adec78dfb804a2e9197292e10e5e18991e73e5bbc"
checked, contract, value, call, program = map(int, facts["abi"].split(","))
native_input = sha(
    b"xray:xir-native-input:v2"
    + struct.pack("<10I", 2, checked, contract, value, call, program, 1, 1, 3, 2)
    + b"".join(sha(v) for v in (b"source", b"closed", b"layout", b"policy", b"generated"))
    + toolchain
)
native_bytes = bytes((0x4d, 0x5a, 0, 0xff, 0x80, 1, 2, 3))
native = sha(native_bytes)
assert native.hex() == "8c8d152837835c3e8c4d0f2b976243db5a7032f8cc68d4b075a121681077409d"
artifact = sha(b"xray:xir-native-artifact:v2" + struct.pack("<IQ", 2, len(native_bytes)) + native_input + native)
for key, value in (("toolchain", toolchain), ("input", native_input), ("native", native), ("artifact", artifact)):
    assert facts[key] == value.hex(), (key, facts[key], value.hex())
print("independent schema2 frames passed; current ABIs:", facts["abi"])
