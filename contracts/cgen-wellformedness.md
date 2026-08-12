# Generated-C well-formedness contract

Status: re-frozen after task 244 added the fail-closed restricted-C90 policy.

Every generated C translation unit is verified before it is written or handed
to a host compiler:

- W1: braces, parentheses, quotes, and comments are balanced.
- W2: generated identifiers contain no path/source-fragment contamination.
- W3: statement-shaped output cannot escape to file scope.
- W4: a generated `vN` temporary is not used before its definition in the
  current function.

The verifier is always on. There is no production bypass or warning-only mode.
A violation is an internal compiler error with a retained diagnostic dump. The
category meanings, priority, and pre-host-compiler enforcement point are
frozen; changes migrate the four injection fixtures and real CGen workloads.

When the explicit restricted-C90 dialect is selected, the same pre-host-
compiler verifier additionally rejects C99/C11 syntax, declaration forms, and
ordinary Xray runtime state outside the frozen scalar freestanding profile.
This policy is separate from W1-W4 and cannot weaken or bypass them. It ignores
tokens inside string and character literals and block comments, but rejects
line comments, compound literals, declaration-after-statement residue, C11
atomics/alignment/thread-local forms, anonymous flexible storage, variadic
declarations, and reachable shared/builtin/dynamic runtime state. Its positive
fixture, forbidden-residue injections, and literal/comment boundary cases are
part of the frozen verifier suite.

## Digest anchors

anchor-sha256: src/aot/xi_cgen_verify_output.h a3662a2575d97d5da3f5ae63fe8319eabc0aad0bc1303264bc8a468397bd9705
anchor-sha256: src/aot/xi_cgen_verify_output.c e981972ae6f97d20d64096009558d889e01b613433f82ebede9f2bbeab8523fa
anchor-sha256: tests/unit/aot/test_cgen_verify_output.c 520622431943c0ff2f1ceba36b1905537dc30f18272f87ce28de5265e0e346d3