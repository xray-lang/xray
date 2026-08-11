# Typed TargetPlan scalar execution contract

TargetPlan schema 5 may carry a canonical per-function instruction table.
Instruction authority is separate from the production AOT family mask: a
verified plan can remain a complete AOT plan while exposing no typed execution
family. A function with zero instruction rows is execution unavailable, never
an empty successful program. The production builder emits a complete group
only for a verified, parameter-free and capture-free, single-return-block
signed-`i64` function whose operations are entirely in the supported family.
Every other function emits zero rows; no partial group or fallback is allowed.

The only supported execution family is a closed straight-line signed `i64`
program consisting of constants, copies, wrapping addition, wrapping
subtraction, wrapping multiplication, and one return. A non-empty function
group must form an exact table partition in canonical function and dense row
order. Its independent verifier requires every referenced slot to belong to
that function and have identical trivial signed-`i64` register and memory
representations. It also proves single assignment, use after definition,
canonical arity and unused fields, and a unique final return. Unknown or
unsupported instructions fail closed.

Instruction rows participate in the TargetPlan fingerprint, the bounded XTP
section directory, the exact 32-byte row codec, and candidate materialization.
The internal scalar dispatcher accepts only an immutable verified plan, its
exact fingerprint, a derived nonzero function execution family, and exact
typed-frame slot identities. It independently recomputes the target-content
fingerprint and does not inspect SemanticPlan or Xi. It has no legacy VM
opcode, `XrValue`, AOT, or generated-C fallback.

This is a test-only scalar execution foundation. It adds no public CLI or
installed execution route and does not claim general typed VM instruction
coverage, control flow, calls, aggregates, ownership, exceptions, coroutines,
or complete typed TargetPlan VM execution.

Evidence:

- `test_typed_dispatch` proves zero-row rejection, wrapping scalar execution,
  production construction and unsupported-function omission,
  SemanticPlan-independent execution, exact XTP roundtrip execution,
  fingerprint/content rejection, and fail-closed mutation of opcode, def-use,
  row identity, function identity, and return structure.
- `test_xtp_format` proves the instruction row width is part of the complete
  exact codec registry.
- `test_typed_frame_runtime_archive` proves the dispatcher and verifier link
  into the runtime-only archive without activating a public execution route.

anchor-sha256: src/plan/target/xr_target_plan.h 966d08684c5c4db79ecd86180d6a5c590bb23a7ae5dd364e87807836bd7278e4
anchor-sha256: src/plan/target/xr_target_plan.c 3a2223d2bc7cf0e9b8152a5a79daea0c79fb9007bafc85b6c1751332f88ae051
anchor-sha256: src/plan/target/xr_target_builder.c 46b935191d1409171a8358e75f2f0b74763fd7a7741cb672f2371027038d9903
anchor-sha256: src/plan/target/xr_target_instruction_verify.h 5eea43c77cf0e3802e30eacf12ca7e1a105b7b32de0497635cf7048de1b3438b
anchor-sha256: src/plan/target/xr_target_instruction_verify.c af74c69df7296ff561d3a3abbf4d42a4016e6db54762a750c6da2ad8b4f2ee07
anchor-sha256: src/plan/target/xr_target_verify.c 19c858372137e59b1eee385ea0ef1e5923e3ad0df2eb99cd1e793eb746a01084
anchor-sha256: src/plan/format/xr_xtp_schema.h cf4a6ddb9385ef378dbea1b9e76e6780e831c026aaa4db5c7f78bf0e99243944
anchor-sha256: src/plan/format/xr_xtp_rows.c 2354fa4354931519ecf8093b27d2ac1107b47fc9691ccf5007d9db381f8fa384
anchor-sha256: src/plan/format/xr_xtp_encode.c 8cb0983494ace434ec1d1f7389f19d4780ad82f6f88460144e04a9e28c1502bc
anchor-sha256: src/plan/target/xr_xtp_materialize.c b8196ae0744dfa56b23c1a0f93e27aabf761340b0b3ad28c45e952b32b69ebe7
anchor-sha256: src/vm/xr_typed_dispatch.h fd64a88c487eb3ffe90e4fb52aee41bf6b3e5ed78f14bf1106e764d789ae558f
anchor-sha256: src/vm/xr_typed_dispatch.c c4d7a76779f0080211e41147d86721e5dbf90edd3db4395d5e3b18fc0640106a
anchor-sha256: src/vm/xr_typed_frame.c f0a3c7ea24cc7b712ac8de2923e92ac8bbb5ddc85006878b147ab9d506fd6ac6
anchor-sha256: tests/unit/vm/test_typed_dispatch.c 52836fa969629698359a0df893581c3341b45b17977990178dbae12edd438f1c
anchor-sha256: tests/unit/plan/test_xtp_format.c 0380e403cfa7c993e5893cfae029932596d0e1ac430b32bae6dc0ad0da894d03
anchor-sha256: tests/unit/runtime/test_typed_frame_runtime_archive.c f3e4f5d2a83f6efbd01e634013a7f213d998d3065e93f6c0419ddae0cd393616
