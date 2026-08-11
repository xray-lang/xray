# Unified target-machine discovery contract

The discovery inventory is the fail-closed migration surface for replacing the
legacy VM and mixed AOT planning model.  It records current implementation
facts; it does not bless those facts as the final architecture.

The generator must derive every Xi operation, every `XaotBundle` plan row, and
every legacy opcode from their current source-of-truth registries.  A new item
that is not classified, assigned a unique future owner, given an independent
oracle, and assigned a migration or deletion task is a contract failure.

Object allocation formulas converge on `XrExtentPlan`.  A universal object-size
field and a dynamic-shape or Json plan family are forbidden.  Diagnostic codes,
entity identifiers, table ordering, fingerprints, integer encodings, and
endianness are stable inputs to later executable schemas.

The support matrix distinguishes supported, CI-only, unqualified, and
unsupported configurations.  A failed row cannot be relabelled as a skipped
configuration.

The baseline policy is the governed source of truth for correctness lanes,
fresh configure/build preflight, clean initial and final source identity,
compiler binary and native toolchain identity, and source-run performance
measurements.  Cold, warm, and one-module-edit samples retain raw wall-time and
peak-RSS evidence; p50, p95, variance, fixture contents, runner contents, and
the policy manifest digest are recomputed by the verifier.  A qualifying run
pins the governed logical CPU, records child-observed or explicitly inherited
affinity, verifies the active host power policy against the policy-owned GUID,
and isolates temporary, cache, and user-home state outside the source tree.

Qualification remains `failed` until a full-scope evidence file and every raw
log digest pass the independent verifier; manifest verification fails closed
when any retained log is absent.  A timeout, malformed result, stale or dirty
identity, high variance, source-root residue, or failed lane remains `failed`;
the protocol has no skip, allowlist, dirty, or fallback state.

anchor-sha256: contracts/target-machine/semantic-owner-inventory.json fb9b0aa82b9d389a5fc2d8364f551cad00ac7622f0097828ec4eef92088de54a
anchor-sha256: contracts/target-machine/aot-plan-destination-inventory.json 08dbb67963521bb1b8e5f58e8c9f06d5f6aea291949a9343a81c74377e692c63
anchor-sha256: contracts/target-machine/legacy-vm-inventory.json 10971e9d85b7c17916f69d7b1b03ff8c4d0f6c35d0af04aa41d14bcf40f944fd
anchor-sha256: contracts/target-machine/legacy-product-residue.json 052cac030a7f91c22c7c4a40e2571f3d9f0a274661aa1396a13f5c5bf4600827
anchor-sha256: contracts/target-machine/object-extent-inventory.json 2a11e03de64c950b017d7033fca6af9728f99008da5270eb9724fa5fd815254f
anchor-sha256: contracts/target-machine/validation-matrix.json 983dd83a1acc22fc66576441dcdfa3b8b52ee606fb6256abefefa2e848310aed
anchor-sha256: contracts/target-machine/baseline-manifest.json 9264ec82b4e58d421c7b285e3bc2ff598ef09587622110de6cf72d79cbf7ddef
anchor-sha256: contracts/target-machine/diagnostic-codes.toml 54ee3affe064aabfb89fe1cc75ef5642837fd14b6e7e575a0badb4fc670efd74
anchor-sha256: contracts/target-machine/id-and-fingerprint-policy.toml df51b24d5ff63004c388dfd7621037d44c20b45ccff29a195680f715b5b7c5e2
anchor-sha256: scripts/target_machine_phase0.py 184fd8aacbcd73130c83a116781dfccdc08d9d5d7b7f8b485f6d2f17e262132e
anchor-sha256: scripts/check_legacy_product_residue.py 0388d636da6384ea62bfaf8401764955541be24b207511727c33af2d85f3a11f
anchor-sha256: scripts/check_runtime_header_dependencies.py 8d27ba12e165ebebed5ab40ce9ad23ea4f66ff8ba5d31f42e41b32938d53831b
anchor-sha256: tests/target-machine/phase0/run_baseline.py 24128c9f2b7a8733488ce03b190eef02965a88050cc915d63296be602a7577b1
anchor-sha256: tests/benchmarks/target-machine/source_run/main.xr ab5ebc43d7c39edc5e2d3c6cee282e4f103e411b52006d21eef8f4e65e4e1b52
anchor-sha256: tests/benchmarks/target-machine/source_run/helper.xr b58a381ae7f36b1533178d9debc359d6386a179cacf828445bd7feaa03824995
anchor-sha256: tests/benchmarks/target-machine/source_run/helper.edited.txt 5e8b2152e52314bf013be19e4c6ebf39051b3dd254301a6df793e9a867d33359
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr 40a77086074f6b47b0ea132b1700c62296b8341208c347066cc6e596082eb920
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/manifest.toml c350e756a3125593e0c89f36f0cb6363bf826b46e3ed0edfbb7426834cdc4837
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/report.json 2f1dd9519207a52797a522c4de4b72051a323af710da53c6c41726a05c56e588
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/mailbox.xr 84d14ec039196012c241ea9083ca86025e5b9e493c049f8a948c4a92f32a25cb
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/manifest.toml 135954b1e3d0e8fec591f04add8f52de869de2c06cba60e0832d2e006391c9ab
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/report.json 240abb288cd49a4522e2a3a84785730bce8f3f76f5da7516c13b0677d0093d51
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/workload.xr 29826802ad6370e69849e55576c68d1d4f90c9f485deca7fcc9615133116f987
anchor-sha256: tests/target-machine/phase0/negative/manifest.toml af0f73be35c7021e07e0473bf080706838f7fbb0d4e147e5cb9e1bc54b98eec4
