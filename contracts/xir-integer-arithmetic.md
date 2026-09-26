# XIR fixed-width integer arithmetic and source contract

Integer types are i64=2, i8=5, i16=6, i32=7, u8=8, u16=9, u32=10 and u64=11.
They remain distinct from bool, rune, floats, target-sized integers and generic
parameters. Signed payloads are sign extended, unsigned payloads zero extended,
and u64 retains all 64 bits. Constants, call arguments/results, cells and output
reject noncanonical narrow payloads before execution. Integer values copy without
allocation and drop to canonical unit; cells retain their existing ownership.

The allocation-free shared runtime explicitly receives width and signedness.
Add/subtract/multiply and bitwise operations reduce modulo the result width.
Signed division truncates toward zero, remainder follows the dividend's sign,
and every signed MIN/-1 wraps to MIN with zero remainder. Unsigned division,
remainder and ordering interpret all bits unsigned. Comparisons return bool;
the shared runtime compares without subtracting. Zero division faults, unwinds
owned frames and skips later writes; initializer failure remains sticky.

All fixed-width shift counts retain the language's modulo-64 rule, including
negative counts. Shifts keep the left type. Narrow left shifts truncate; right
shifts beyond the width produce zero or signed -1. Thus u8(1) << 8 is zero and
u8(1) << 64 is one. Only unsigned host shifts execute. Explicit conversion
validates the source and reduces its mathematical value modulo the target width.
No host signed overflow or implementation-defined unsigned-to-signed cast is used.
Runtime failure clears the numeric output and returns its status.

CONST_INT and the ADD/SUB/MUL/DIV/REM/AND/OR/XOR/SHL/SHR/EQ/NE/LT/LE/GT/GE_INT
family atomically replace the former i64-specific names, with no alias. The new
CONVERT_INT operation has one concrete integer operand and result. Arithmetic
operands/results share an exact integer type; comparisons require two equal
integer types and return bool; shifts allow any integer count type. Every use
retains SSA role and dominance checks. Unconstrained T gains no operation or
conversion permission through specialization. Copy, phi, call, module slot,
cell and callable metadata preserve the exact integer type.

Storage width/alignment is 1/2/4/8 bytes. SSA and frame payloads stay 8-byte
canonical slots; boxed/parameter/result boundaries are 16 bytes aligned to 8.
Unsigned output renders full unsigned decimal, including 18446744073709551615
for all-ones u64. Groups validate and render entirely under budget before one
sink publication. Display does not change the stored integer type.

The source producer admits all eight names and concrete expr as T through
CONVERT_INT, rejecting nullable casts and bool/rune/float or unconstrained-T
conversion. Binary evaluation remains once, left to right. Already typed
same-signedness arithmetic operands widen explicitly to the wider type before
the unique operation; shifts do not widen the left. Unary negation/complement
and ++/-- retain the operand/binding type. Compound assignment reads the binding
before its RHS, must retain the binding type, and stores only on success. Exact
typed declarations, module state, closure cells, generic copy/call and print use
the same pipeline. Full literal contexts and contextual widening at assignment,
return and parameter sites remain open producer work. Explicit casts provide
executable witnesses without claiming those missing contexts. Floating point,
checked/saturating library methods and numeric containers remain unqualified.

Value7/Call11/Program6 and Checked schema4/semantic12 are the sole admission
versions. Old packets and native entries reject without readers/adapters. VM,
portable generated-C leaf and resumable native consumers share the Lowered
integer contract and runtime. Source witnesses and hand-authored packet fixtures
are tested independently against fixed expected values.

verification-test: test_xir_execution
verification-test: test_xir_native
verification-test: test_xir_checked
verification-test: test_xir_calls
verification-test: test_xir_call_native
verification-test: test_xir_source
verification-test: test_xir_packet_vm
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_admission
