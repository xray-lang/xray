# XIR binary floating values and execution

The runtime-neutral floating core accepts explicit IEEE binary32 or binary64
bit patterns, with binary32 zero extended to 64 bits. Unsupported widths or
nonzero high binary32 bits reject. No allocation, host floating arithmetic,
floating cast, process rounding-mode mutation or legacy scalar tag is involved.

Integer-to-float and binary64-to-binary32 conversions round to nearest, ties to
even. Finite overflow becomes signed infinity. Binary32-to-binary64 is exact.
Subnormals are preserved and rounded with gradual underflow; zero preserves its
sign. All NaN inputs (including signaling and signed payloads) convert to the
positive canonical quiet NaN: 0x7fc00000 or 0x7ff8000000000000. Conversion to the
same float width still canonicalizes NaN. Host floating exception flags and
rounding state remain unchanged.

Integer inputs use the existing explicit width/signedness and canonical payload
contract. Float-to-integer first truncates toward zero, then checks the complete
target range before producing a canonical integer payload. Negative fractions
between -1 and zero may produce unsigned zero; negative integral values cannot.
NaN, infinity and finite out-of-range results return RANGE. Invalid encodings or
formats return BAD_ARGUMENT. Every failure with an output pointer clears it;
null output pointers reject. Integer input/output paths preserve all u64 bits.

Comparison reports less/equal/greater/unordered without subtracting or using
host floating instructions. Either NaN makes the comparison unordered, +0 and
-0 compare equal, and infinities/subnormals retain IEEE ordering. Negation flips
the sign of every non-NaN value and canonicalizes NaN instead of leaking payload
or sign. These helpers define the execution boundary for both VM
and generated native code.

XIR admits f32 (type 12) and f64 (type 13). Values and CONST_FLOAT require
canonical positive quiet NaN, with binary32 zero extended; all other IEEE
encodings including signed zero and infinities are admitted. Scalar storage is
4/8 bytes, SSA and frame payloads are 8 bytes, boxed/parameter/result values
are 16 bytes aligned to 8. Copy, local/cell storage, module slots, generic
substitution, callables and suspension preserve exact bits and concrete type.

CONVERT_NUMBER replaces the former integer-only conversion without an alias. It accepts any concrete
integer/float pair, retaining the integer narrowing/wrapping contract. Floating
conversion uses the rules above; out-of-range float-to-integer raises a distinct
NUMERIC_RANGE execution fault with canonical unit result and normal cleanup.
NEG_FLOAT and six float comparisons accept equal-width concrete float operands;
NaN makes EQ false, NE true, and every ordered relation false. No host FPU state
changes occur. Checked schema6/semantic17, Value10/Call14/Program11 reject older
versions, including otherwise integer-only artifacts.

Source admits f32/f64 annotations, explicit numeric casts, float negation and
comparisons. The only implicit floating conversion is f32 to f64, including
conditional joins. Already typed integer/float mixing requires explicit casts. Ordinary
generic bodies cannot perform numeric operations without concrete proof.
Decimal literals and exact integer contextual admission follow
`xir-decimal-literals.md`. Floating typed output follows the formatting contract below. No product
or cross-platform qualification is implied by this subset.

verification-test: xir_float_vectors_check
verification-test: xir_float_arithmetic_vectors_check
verification-test: xir_float_format_vectors_check
verification-test: test_xir_execution
verification-test: test_xir_native
verification-test: test_xir_checked
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_admission
verification-test: test_xir_packet_vm

## Arithmetic kernel contract

The operation family is binary32/binary64 addition, subtraction,
multiplication and division. Qualification remains OPEN until the rebuilt
kernel, pipeline and source tests pass; typed output requires its own contract.

`xr_xir_float_arithmetic` takes a width, an ADD/SUBTRACT/MULTIPLY/DIVIDE selector,
two explicit bit patterns and an output pointer. It uses the same width and
encoding validation as conversion. Unknown selectors and null outputs reject;
all other failures clear a nonnull output. Raw NaN operands are canonicalized
by the kernel; normal XIR value admission still rejects noncanonical NaNs.
The helper neither allocates nor reads or modifies host floating-point state.

Each operation rounds its exact mathematical result once at the original
operand width, nearest with ties to even. Finite overflow produces signed
infinity; underflow is gradual. Subtraction is addition with the right sign
inverted. Exact cancellation produces positive zero, except addition of two
negative zeros produces negative zero. Multiplication and division use the
exclusive-or operand sign, including zero and infinity results. A finite
nonzero value divided by zero yields signed infinity rather than an integer
division fault. Zero divided by zero, infinity divided by infinity, zero times
infinity, opposite infinities added, and every NaN operand produce the unique
positive canonical quiet NaN. No contraction or operation reassociation occurs.

Integer guard/round/sticky information must survive exponent alignment,
wide multiplication and quotient remainder handling. Binary64 multiplication
needs the complete 106-bit significand product before final rounding. A host
double intermediate does not implement binary32 operation semantics.

Verification must include independent exact-rational bit expectations for
both widths and all four operations, signs of zero, subnormal boundaries,
overflow ties, cancellation and invalid encodings, under all four host rounding
modes with exception flags preserved. Pinned upstream vectors are additional
evidence, not a product dependency or a substitute for Xray execution tests.

## Arithmetic pipeline and source contract

Append ADD_FLOAT=70, SUB_FLOAT=71, MUL_FLOAT=72 and DIV_FLOAT=73, introduced at
OP_COUNT=74; the current table also includes the separately governed struct operations. All have stage mask 7, two operands, no successors, zero immediate
and unused fields, and a FLOAT result with both operand types exactly equal to
that result. A type parameter cannot satisfy FLOAT merely through marker
constraints. Checked specialization uses these same rules; no later executor
may reinterpret or admit a rejected instruction. Arithmetic was introduced at semantic16; current admission requires
schema6/semantic17 and Value10/Call14/Program11. Older semantic packets
reject before use, including artifacts without arithmetic instructions.

Source admits `+ - * /` and corresponding compound assignments for concrete
f32/f64 operands. A mixed pair widens f32 to f64 before the operation; direct
literals retain the existing contextual admission rules. Typed integer/float
mixing and float remainder or bitwise operations reject. Ordinary generic
definitions require concrete operand proof rather than testing future instances.
Compound assignment reads the left value before evaluating the right and
commits only after the right completes; its result cannot narrow implicitly.
The VM and both generated-C entry forms call the one integer-only kernel.

## Typed decimal output contract

Finite nonzero output uses the fewest significant decimal digits that round
back to the original binary32/binary64 value under RNE. Among equal-length
candidates choose the closest exact decimal value; a midpoint chooses an even
last digit. Binary32 is formatted at its own precision, not after widening.
The shortest significand has no trailing zeros. If its decimal exponent k is
in [-4,16), use ordinary decimal notation, with `.0` for an integral result.
Otherwise use one leading digit, a decimal point only when more digits follow,
lowercase `e`, an explicit exponent sign and no redundant exponent zeros.
Special values are exactly `nan`, `inf`, `-inf`, `0.0` and `-0.0`.
All bytes are ASCII and independent of locale and host FPU state.

`xr_xir_float_format(width, bits, bytes, capacity, length)` writes a terminated
string and reports its length excluding the terminator. Width/encoding checks
match the raw numeric kernel. A nonnull length is cleared on failure; a nonnull
buffer with positive capacity starts empty on failure. Null required pointers
or insufficient capacity reject without publishing partial text. Formatting
has fixed bounded integer workspace and allocates no memory. A 32-byte buffer
suffices for all admitted values including sign and terminator.

PRINT/OUTPUT admit canonical floating values through the existing typed output
group. Source print remains an ordinary resolved call. All argument evaluation
precedes rendering; the complete group is budgeted before one sink publication.
Malformed values, buffer exhaustion, allocation failure or rejected output
retain existing OUTPUT_ERROR/cleanup semantics and never publish a partial
group. This changes admission and bytes, not the Value10/Call14/Program11 layout.
The semantic-16 family includes this output admission.

## Complete source execution evidence

The two-module floating fixture must independently match typed output bytes and
result bits in source VM, parser-free Checked VM, native AOT and both mixed
module directions. Tests exercise per-instance root state and imported constants, captured mutable
f32 cells, compound read-before-suspending-RHS, wakeup identity and cancellation.
Owned string results survive instance and Program destruction. Every observed
runtime allocation failure must unwind without partial output groups or live
ownership; this is required in addition to numerical kernel vectors.

verification-test: test_xir_source_floats
verification-test: test_xir_float_packet_vm
verification-test: test_xir_source_float_native
verification-test: test_xir_source_float_mixed
