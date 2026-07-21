# Generated-C well-formedness contract

Status: frozen by task 220.

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

## Digest anchors

anchor-sha256: src/aot/xi_cgen_verify_output.h 66f14bef44aa092f2e65c0f6bfc72a5bca6dd919e5465453f60b0500277a5d92
anchor-sha256: src/aot/xi_cgen_verify_output.c fbc1e7d0df1923b028bdba86bf770c4c2003dee57da95166a138939d972374cc
anchor-sha256: tests/unit/aot/test_cgen_verify_output.c 6590567d007104b306979f643722208265dcbda458d2a38373d71981034c958b
