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
changes occur. Checked schema5/semantic15, Value9/Call14/Program9 reject older
versions, including otherwise integer-only artifacts.

Source admits f32/f64 annotations, explicit numeric casts, float negation and
comparisons. The only implicit floating conversion is f32 to f64, including
conditional joins. Already typed integer/float mixing requires explicit casts. Ordinary
generic bodies cannot perform numeric operations without concrete proof.
Decimal literals and exact integer contextual admission follow
`xir-decimal-literals.md`. Floating arithmetic and floating typed output remain
rejected until their arithmetic/formatting contracts are implemented. No product
or cross-platform qualification is implied by this subset.

verification-test: xir_float_vectors_check
verification-test: test_xir_execution
verification-test: test_xir_native
verification-test: test_xir_checked
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_admission
verification-test: test_xir_packet_vm
