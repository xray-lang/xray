# Xray semantic contracts

This directory is the machine-checked semantic contract layer introduced by
task 220. Language syntax and public API shape may still change directly; the
contracts below freeze the meaning of compiler/runtime invariants and their
verification protocols.

Contracts transfer verification responsibility to existing behavior, ABI and
rejection tests using one record per test:

```text
verification-test: registered_ctest_name
```

`scripts/check_contract_freeze.py --list-tests` projects this owner list into
CTest fixtures. `ctest -R contract_freeze` automatically runs each named test
once; a failed assertion blocks the dependent contract gate. A full CTest run
uses those same tests, without a second matrix. Missing, disabled, skip-enabled
or disconnected tests fail registration validation. Registration alone is not
proof that the test ran successfully.

Contracts whose replacement assertions still fail retain `anchor-sha256`
records and their registered source sets. Repair and rerun those assertions
before removing that contract's digest protection. The checker still rejects
remaining digest drift. This is a per-contract transition, not an option to
choose a weaker gate. Source changes do not require hashes after transfer;
runtime Program/schema/provider/ABI/artifact identities remain mandatory.

Existing runners bind evidence to source, binary and content identities.
Contract changes and evidence retirement remain reviewable in ordinary commit
messages and the governed tombstone inventory. No test is retired merely
because its source digest is retired. Overall qualification still requires
all applicable regression, native, safety and platform gates.

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
- `structural-object-json-map-boundary.md`
- `sort-semantics.md`
- `semantic-ownership.md`
- `semantic-performance-budget.toml`
- `semantic-performance-baseline.json`
- `semantic-runtime-benchmark.json`
- `unified-target-machine-discovery.md`
- `execution-error-publication.md`
