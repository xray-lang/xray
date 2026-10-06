"""Derive fixed Task Unit Checked roles without running a product writer."""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / "tests/unit/xir"
def historical(path, name):
    source = path.read_text()
    match = re.search(r"static const uint8_t " + name + r"\[\] = \{(.*?)\};", source, re.S)
    assert match
    packet = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    assert packet[:8] == b"XRCHK\0\0\0"
    assert struct.unpack_from("<IIIIQ", packet, 8) == (25, 66, 2, 0, len(packet) - 64)
    assert hashlib.sha256(packet[:32] + packet[64:]).digest() == packet[32:64]
    return packet

def frame(body, contract):
    prefix = b"XRCHK\0\0\0" + struct.pack("<IIIIQ", 25, contract, 2, 0, len(body))
    return prefix + hashlib.sha256(prefix + body).digest() + body

def u32(*values): return struct.pack("<" + "I" * len(values), *values)

def fixed_types(element):
    # One Unit-returning function, parameters Task<element> and Task<string>.
    # No ordinary Unit parameter, generic argument, declaration or SSA result.
    body = u32(0, 1, 0) + u32(9) + b"taskTypes" + u32(2, 256, 257, 0)
    body += u32(1, 0, 1, 0, 0, 1)
    # Existing RETURN ordinal 33, Unit type, unused operands/targets/immediate/typeargs zero.
    body += u32(33, 0, 0, 0, 0, 0) + struct.pack("<Q", 0) + u32(0, 0)
    body += u32(0, 0, 2, 0, 0, 8, 0, element, 8, 0, 3, 0, 0)
    return body

def header(path, name, packet):
    lines = ["/* Independent named Checked 25/67 roles; historical complete66 bytes remain separate. */",
             "static const uint8_t " + name + "[] = {"]
    lines += ["    " + ",".join("0x%02x" % b for b in packet[i:i+12]) + "," for i in range(0, len(packet), 12)]
    path.write_text("\n".join(lines + ["};", ""]))

old_go = historical(TEST / "xir_task_go66_golden.h", "task_go66_golden")
old_types = historical(TEST / "xir_task_types66_golden.h", "task_types66_golden")
assert frame(fixed_types(2), 66) == old_types
current_go = frame(old_go[64:], 67)
current_types = frame(fixed_types(2), 67)
current_unit = frame(fixed_types(0), 67)
assert current_go[64:] == old_go[64:]
assert len(current_unit) == len(old_types) == 221
header(TEST / "xir_task_go67_golden.h", "task_go67_golden", current_go)
header(TEST / "xir_task_types67_golden.h", "task_types67_golden", current_types)
header(TEST / "xir_task_unit67_golden.h", "task_unit67_golden", current_unit)
role_names = ["scalar_go", "initializer_target", "await_place", "go_target", "await_binding", "mutable_worker", "cleanup_helper"]
old_bad = {name: historical(TEST / "xir_task_go66_golden.h", "task_go66_" + name) for name in role_names}
for name, packet in old_bad.items():
    temporary = TEST / (".task_go67_" + name)
    header(temporary, "task_go67_" + name, frame(packet[64:], 67))
    with (TEST / "xir_task_go67_golden.h").open("a") as destination: destination.write(temporary.read_text())
    temporary.unlink()
rows = [dict(role="existing_go_same_body", old_sha256=hashlib.sha256(old_go).hexdigest(),
             current_sha256=hashlib.sha256(current_go).hexdigest(), body_sha256=hashlib.sha256(old_go[64:]).hexdigest(),
             length=len(old_go), body_equal=True),
        dict(role="Task_Unit_and_string_parameters_Unit_RETURN", old_sha256=hashlib.sha256(old_types).hexdigest(),
             current_sha256=hashlib.sha256(current_unit).hexdigest(), length=221,
             old_named_encoder_exact=True, element_change="first Task descriptor I64 2 -> Unit 0", opcode_RETURN=33)]
rows += [dict(role="existing_Task_i64_and_string", old_sha256=hashlib.sha256(old_types).hexdigest(), current_sha256=hashlib.sha256(current_types).hexdigest(), body_equal=current_types[64:]==old_types[64:], length=len(current_types))]
rows += [dict(role="current_bad_"+name, old_sha256=hashlib.sha256(packet).hexdigest(), current_sha256=hashlib.sha256(frame(packet[64:],67)).hexdigest(), body_equal=True, length=len(packet)) for name,packet in old_bad.items()]
print(json.dumps(rows, indent=2))
