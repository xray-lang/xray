# Compiler meta-ownership contract

Status: frozen by task 220.

- R-OWN-1: IR, evidence, plan, and CGen metadata must not retain unowned AST or
  analyzer strings across their source lifetime.
- R-OWN-2: code must not retain a pointer into a growable array across an
  append, grow, or reallocation of that array.
- R-OWN-3: CGen context names crossing generation stages are arena/pool/static
  owned, or carry a precise reviewed ownership justification.

The inventory categories `AST_PTR_INTO_IR`, `PTR_ACROSS_GROWTH`, and
`CGEN_BORROWED_NAME` are zero, fail-closed gates. Category boundaries and the
meaning of an `owned:` exemption are frozen. Suppression budgets may shrink;
expanding one requires a contract change and evidence explaining why ownership
cannot be made explicit.

## Digest anchors

anchor-sha256: scripts/check_meta_ownership.py bb366fbfdf03b975b45d408f106dc105fd50a65a7111591e77bc4c7dca5fda6e
