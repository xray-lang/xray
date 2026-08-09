# Xray semantic contracts

This directory is the machine-checked semantic contract layer introduced by
task 220. Language syntax and public API shape may still change directly; the
contracts below freeze the meaning of compiler/runtime invariants and their
verification protocols.

Every contract records SHA-256 anchors in this form:

```text
anchor-sha256: path/from/repository/root digest
```

`scripts/check_contract_freeze.py` verifies the anchors in CTest. A change to
an anchor or contract file does not require a dedicated commit trailer. The
ordinary self-contained commit subject/body and governed evidence must explain
how affected differential cases, KATs, shape gates, ports, or other evidence
were rerun, regenerated, or retired. Retired evidence belongs in its governed
tombstone inventory; it must not silently disappear.

Initial frozen contracts:

- `intrinsic-identity.md`
- `xi-canonical-ops.md`
- `effect-semantics.md`
- `zero-cost-residue.md`
- `rc-contract.md`
- `cgen-wellformedness.md`
- `meta-ownership.md`
- `differential-protocol.md`
- `target-abi.md`
- `object-json-domain.md`
- `sort-semantics.md`
- `semantic-ownership.md`
- `semantic-performance-budget.toml`
- `semantic-performance-baseline.json`
- `semantic-runtime-benchmark.json`
