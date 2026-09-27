# Exact decimal literal admission

Decimal floating tokens retain an AST-owned exact spelling and length. Parsing
does not truncate a token, use strtod, consult locale, alter rounding mode or
raise host floating exceptions. The common integer-only converter rounds the
exact decimal rational once to binary32 or binary64, nearest with ties to even.
Overflow becomes signed infinity; gradual underflow preserves signed zero.
The converter accepts an optional sign, digits with an optional decimal point,
an optional e/E exponent, and underscores strictly between digits. Empty input,
missing exponent digits, misplaced separators, non-ASCII syntax, unsupported
widths and null outputs reject; a nonnull output is zero on failure.
The current source lexer admits digit-led forms with digits after a decimal
point, or an exponent. Leading-dot and trailing-dot source grammar remains
unqualified; the shared converter accepting those forms does not qualify it.

A 1152-significant-digit prefix plus a nonzero sticky tail suffices: a binary64
rounding boundary has a denominator dividing 2^1075 and at most 54 significand
bits, hence fewer than 770 significant decimal digits. All characters still
participate in syntax validation. Fixed 192-limb integer storage covers the
bounded rational after proven decimal magnitude shortcuts; no heap allocation
or host floating approximation supplies a rounding decision.

XIR source floating literals default to f64 without a unique floating context.
Annotations, assignments, declared returns and substituted call parameters
select their f32/f64 precision directly. Grouping and one direct negation keep
that context. An already formed f64 expression never narrows implicitly. A
direct integer in a unique floating context is accepted only if its entire
mathematical value is exactly representable; explicit casts continue to round.
Neither generic instantiation nor reflection acquires conversion permissions.

The parser's ordinary f64 AST value and XIR's target-precision literal use the
same exact converter. AST cloning copies spelling into the destination owner;
formatting preserves source spelling. There is no compatibility parser or
product execution path. AST structural signatures include the owned spelling
so distinct f32 rounding inputs cannot collapse through their common f64 value.
Built emits the existing canonical CONST_FLOAT; the
Checked/Lowered pipeline and Value9/Call13/Program8/schema5/semantic14 remain
the only execution admission. Arithmetic and floating output stay unqualified.

verification-test: test_xir_execution
verification-test: test_xir_native
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_source_admission
verification-test: test_xir_packet_vm
verification-test: test_parser
verification-test: test_mono
verification-test: test_formatter_strings
verification-test: test_fmt_roundtrip
verification-test: xir_decimal_vectors_check
