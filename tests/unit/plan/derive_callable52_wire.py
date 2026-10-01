"""Independent fixed-width XSM frames for the complete 51/52 probe fixture."""
from pathlib import Path
import hashlib, json, struct
ROOT=Path(__file__).resolve().parents[3]
DATA=Path(__file__).with_name("callable52_independent_fixture.json")
HEADER=Path(__file__).with_name("callable52_independent_wire.h")
def integer(n,width):return int(n & ((1<<(8*width))-1)).to_bytes(width,"little")
def text(v):
 raw=v.encode("utf-8");return integer(len(raw),4)+raw
def fields(row, grammar):
 result=bytearray()
 for item in grammar.split():
  name,width=item.split(":");result+=integer(row.get(name,0),int(width))
 return result
FN="return_type:4 parent:4 parameter_begin:4 parameter_count:2 child_count:2 capture_begin:4 capture_count:2 reserved_capture:2 block_begin:4 block_count:4 value_begin:4 value_count:4 semantic_effects:4 capability_mask:4 source_class:4 source_member_ordinal:2 return_parameter:2 return_provenance:1 source_kind:1 flags:1 is_module_initializer:1 carries_coroutine_ops:1 is_external_entry:1"
TAIL="callable_type:4 unknown_semantic_effects:4 effect_unknown_reasons:4 effect_complete:1"
TY="kind:4 builtin_type:4 source_class:4 child_begin:4 aggregate_extent:4 aggregate_align:4 enum_layout_id:4 child_count:2 enum_member_count:2 scalar_rep:1 flags:1 enum_flags:1 reserved_enum:1"
BLOCK="function:4 operation_begin:4 operation_count:4 predecessor_begin:4 predecessor_count:2 kind:2 successor_0:4 successor_1:4 control_value:4 source_line:4"
OP1="function:4 block:4 result_value:4 result_type:4 operand_begin:4 operand_count:2 opcode:2 metadata_begin:4 metadata_count:2 auxiliary_kind:1 import_resolution:1 effects:4 source_line:4"
OP2="source_start_line:4 source_start_column:4 source_end_line:4 source_end_column:4 source_discriminator:4 semantic_immediate:8 constant:4 callable_function:4"
OP3="ownership_use:1 result_ownership:1 transfer_mode:1 parameter_mode:1 parameter_ownership:1 flags:1 result_alias_operand:2 return_parameter:2 return_provenance:1 return_complete:1 view_source_value:4 view_element_type:4 view_source_operand:2 view_source_parameter:2 intrinsic_kind:1 view_origin:1 view_capability:1 view_lifetime:1 view_complete:1 array_element_storage:1 reserved_view_0:1 reserved_view_1:1 array_hof_kind:1 array_result_element_storage:1"
def make_wire(schema,record):
 p=record["plan"]; count=p["counts"]; owner=p["ownership"]
 payload=bytearray()
 names="type source_class source_method function block operation call_target dependency source_export edge constant entity".split()
 for name in names:payload+=integer(count[name],4)
 for name in ["type_child","parameter","capture","predecessor","operand","metadata"]:
  payload+=integer(count.get(name,p["auxiliary_counts"].get(name,0)),4)
 for name in ["owner","event","edge_state","loop_invariant"]:
  payload+=integer(owner["counts"][name],4)
 payload+=bytes(5*4) # five program binding counts, no PSC in this fixture
 payload+=bytes(12*4+32+16+16) # fully empty program provenance
 offsets=[]
 def begin(kind,row):
  offsets.append(dict(kind=kind,begin=152+len(payload),key=row.get("key","")))
 for e in p["entities"]:
  begin("entity",e);payload+=bytes.fromhex(e["id"])+text(e["key"])
  payload+=fields(e,"parent:4 subject:4 ordinal:4 kind:2 subject_kind:1 flags:1")
 for ty in p["types"]:
  begin("type",ty);f=ty["fields"]
  payload+=bytes.fromhex(ty["id"])+text(ty["key"])
  payload+=bytes.fromhex(f["source_enum_identity"])+text(f["source_enum_key"])
  payload+=fields(f,"kind:4 builtin_type:4 source_class:4")+bytes.fromhex(f["source_class_identity"])
  payload+=fields(f,"child_begin:4 aggregate_extent:4 aggregate_align:4 enum_layout_id:4 child_count:2 enum_member_count:2 scalar_rep:1 flags:1 enum_flags:1 reserved_enum:1")
 fn=p["function"];begin("function",fn)
 payload+=bytes.fromhex(fn["id"])+text(fn["key"])+text(fn["name"])+fields(fn["fields"],FN)
 tail_offset=152+len(payload)
 if schema>=52:payload+=fields(fn["fields"],TAIL)
 b=p["block"];begin("block",b)
 payload+=bytes.fromhex(b["id"])+text(b["key"])+fields(b["fields"],BLOCK)
 for op in p["operations"]:
  begin("operation",op);f=op["fields"]
  payload+=bytes.fromhex(op["id"])+bytes.fromhex(f["allocation_id"])+text(op["key"])+text(f["allocation_key"])
  payload+=fields(f,OP1)+text(f["source_file"])+fields(f,OP2)
  payload+=b"".join(integer(f["evidence_"+str(n)],4) for n in range(8))
  payload+=fields(f,OP3)
 for c in p["constants"]:
  begin("constant",c)
  payload+=fields(c,"type:4 kind:1 integer:8 float_bits:8")+text(c["string"])
 o=owner["owner"];begin("owner",o)
 payload+=bytes.fromhex(o["id"])+text(o["key"])+fields(o,"function:4 origin_value:4 initial_state:1 exit_state:1 return_provenance:1 flags:1")
 e=owner["edge_state"];begin("edge-state",e)
 payload+=fields(e,"owner:4 block:4 successor:4 entry_balance:4 exit_balance:4 entry_state:1 exit_state:1 flags:2")
 # Registry hashes and semantic fingerprint came from independent source framing.
 raw=bytes.fromhex(p["preimage"]); prefix=raw.index(b"\0")+1
 opfingerprint=raw[prefix+16:prefix+48]
 assert len(opfingerprint)==32 and raw[prefix+8:prefix+16]==integer(32,8)
 header=b"XRAYXSM\0"+integer(schema,4)+integer(152,4)+integer(len(payload),8)
 header+=hashlib.sha256(payload).digest()+opfingerprint+bytes.fromhex(record["registry_fingerprint"])+bytes.fromhex(p["fingerprint"])
 assert len(header)==152
 wire=header+payload
 return wire,dict(schema=schema,header_size=152,payload_size=len(payload),artifact_size=len(wire),
                  function_tail_offset=tail_offset,function_tail_bytes=13 if schema>=52 else 0,
                  payload_sha256=hashlib.sha256(payload).hexdigest(),artifact_sha256=hashlib.sha256(wire).hexdigest(),records=offsets)
def generated(data):
 records={}; arrays=[]
 for schema in (51,52):
  wire, record=make_wire(schema,data[str(schema)]);records[str(schema)]=record
  arrays.append("static const uint8_t k_callable%d_wire[] = {\n"%schema)
  arrays.extend("    "+", ".join("0x%02x"%b for b in wire[i:i+32])+",\n" for i in range(0,len(wire),32))
  arrays.append("};\n#define CALLABLE%d_WIRE_SIZE %du\n"%(schema,len(wire)))
  arrays.append('_Static_assert(sizeof(k_callable%d_wire) == CALLABLE%d_WIRE_SIZE, "independent vector size");\n'%(schema,schema))
 return "".join(arrays),records
def main():
 import argparse
 parser=argparse.ArgumentParser();parser.add_argument("--write",action="store_true");args=parser.parse_args()
 data=json.loads(DATA.read_text())
 content,records=generated(data)
 banner="""/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * callable52_independent_wire.h - Independent complete fixed-width probe bytes
 */
#ifndef CALLABLE52_INDEPENDENT_WIRE_H
#define CALLABLE52_INDEPENDENT_WIRE_H
#include <stdint.h>
"""
 content=banner+content+"#endif  // CALLABLE52_INDEPENDENT_WIRE_H\n"
 if args.write:
  HEADER.write_text(content,newline="\n")
  DATA.with_name("callable52_wire_offsets.json").write_text(json.dumps(records,indent=2)+"\n")
 else:
  assert HEADER.read_text()==content
  assert json.loads(DATA.with_name("callable52_wire_offsets.json").read_text())==records
 for schema,r in records.items():print("PASS independent complete",schema,"bytes",r["artifact_size"],"tail",r["function_tail_offset"],r["function_tail_bytes"],"sha",r["artifact_sha256"])
if __name__=="__main__":main()
