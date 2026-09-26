# XIR binary floating conversion foundation

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
or sign. These helpers define a single future execution boundary for both VM
and generated native code.

This contract qualifies conversion/comparison/negation foundation only. It does
not admit float types into XIR values, packets, source, arithmetic, layout or
typed output yet. Existing integer Value7/Call11/Program6 and Checked
schema4/semantic12 remain unchanged. Full floating arithmetic, decimal parsing
and formatting, math methods, XIR integration and cross-platform qualification
remain open. No compatibility path is introduced.

verification-test: test_xir_execution
verification-test: test_xir_native
