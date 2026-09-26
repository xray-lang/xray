# XIR i64 arithmetic and relational contract

The concrete i64 source family follows the language integer contract: +, -, *,
unary negation, ++ and -- wrap modulo 2^64 in every build configuration. This
atomically supersedes the earlier internal checked-addition subset. No compatibility
mode or checked-add alias remains. Explicit checked/saturating library methods are
separate operations and are not admitted by this source subset yet.

Both binary operands evaluate once, left to right. The result of arithmetic is
exact i64. Comparisons ==, !=, <, <=, >, >= produce canonical bool and compare signed
values without subtraction. Generic parameters gain no arithmetic capability from
concrete specialization; missing constraints reject at definition.

Division truncates toward zero. Nonzero remainder has the dividend's sign.
INT64_MIN / -1 is INT64_MIN under the wrapping rule; INT64_MIN % -1 is zero.
A zero divisor for either / or % faults with DIVIDE_BY_ZERO, leaves no result,
unwinds all owned frames, and skips subsequent writes. A function-call fault does
not poison an already initialized Instance; an initializer fault remains sticky.

ADD_I64 and the appended SUB_I64, MUL_I64, DIV_I64, REM_I64 operations use a shared
portable scalar runtime. Unsigned arithmetic and an in-range signed reconstruction
avoid signed overflow and implementation-defined unsigned-to-signed conversion.
Division guards zero and the signed minimum/-1 pair before host / or %.
NE_I64, LE_I64, GT_I64 and GE_I64 compare signed operands directly. Unary negation
lowers to zero minus operand; no new unary operation or alternate execution path.

AND_I64, OR_I64 and XOR_I64 operate on the full 64-bit two's-complement pattern.
SHL_I64 drops high bits and fills low bits with zero. SHR_I64 is arithmetic right
shift, filling with the sign bit. Both counts are normalized modulo 64, including
negative counts: -1 selects 63, -64 selects 0. The runtime shifts unsigned values
only; a negative right-shift input uses complement/shift/complement so host signed
right-shift behavior never defines the language. A zero effective count is identity.
Unary ~ lowers to xor with -1 and produces i64. Bool/string/Atomic and unconstrained
T reject in the definition. Variable compound assignments +=, -=, *=, /=, %=, &=, |=, ^=, <<= and >>= read
the binding once before evaluating the RHS, compute, then store and return the new
value. Failure skips the store. String += uses the existing owned concat/snapshot
contract; all other admitted compound operators require i64. Const/read bindings,
member/index targets and unsupported type pairs reject. These use the same operators
and typed places; no wrapping/shift flags or alternate implementation is retained.

Checked schema is 3; semantic contract is 8 and call ABI is 7. Old semantic
packets and call entries reject before execution, without an old reader/adapter.
Value ABI 4 and Program ABI 2 follow the owned function-value boundary. Compiler-generated C binds call ABI 7.
The scalar leaf and resumable entry paths, packet consumer and source producer
must all use this same contract. Wider numeric families remain unqualified.

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
