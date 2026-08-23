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
The caller selects one exact build root.  Its physical path is never a policy
constant: the runner reconfigures and builds that root with the policy-owned
preset, cache variables, and parallelism, then records `${BUILD_ROOT}` and
`${SOURCE_ROOT}` placeholders plus the verified configuration digest.  No
default-directory retry, sibling-directory scan, or alternate reader is
permitted.  Evidence from the wrong source root, generator, build type,
compile-command setting, or stdlib fastpath setting is rejected before lanes
can qualify.

Qualification remains `failed` until a full-scope evidence file and every raw
log digest pass the independent verifier; manifest verification fails closed
when any retained log is absent.  A timeout, malformed result, stale or dirty
identity, high variance, source-root residue, or failed lane remains `failed`;
the protocol has no skip, allowlist, dirty, or fallback state.
The three governed performance fixture files are explicitly checked out with
LF bytes on every host.  Their policy and contract digests therefore identify
the exact executed input instead of the caller's `core.autocrlf` preference.

anchor-sha256: .gitattributes e978a1bddfdafc2f706780f7bd9a0cca60ba71fb72fcb17a1ef782dacdd3d549
anchor-sha256: contracts/target-machine/semantic-owner-inventory.json 101eae2cee322a6a9252b5cdc20f069063832d35a968837f48edc7e3c7263819
anchor-sha256: contracts/target-machine/aot-plan-destination-inventory.json 81248afac3e1a614cbf6872056e5f68c7adaa83dfc7819400a7fe85842ac97f8
anchor-sha256: contracts/target-machine/legacy-vm-inventory.json 6a3a166c995995fcf56d9bb9a0693a3a7428f28c87d50d410641c9f7cb310d07
anchor-sha256: contracts/target-machine/legacy-product-residue.json ddbc46a5e2a8c1f88e8707ec6b7e917b1e58ef4f23a565238c70b24889a1f358
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: contracts/target-machine/object-extent-inventory.json 6e38c6eef7295f27026e58d4ad4b4fc5165ef4a7f3c86ba1e9200397e4e86cc8
anchor-sha256: contracts/target-machine/validation-matrix.json 3dc4e72f21eea5fe0f76efbe4bde8c6022540b077a2f5625764f6102cab97300
anchor-sha256: contracts/target-machine/baseline-manifest.json db89eb653fd5e47fd28bbbded0842ce859fdf58e14bf8140ce3872df9b8d1474
anchor-sha256: contracts/target-machine/diagnostic-codes.toml f4cea43f422ccd0a5e336922eca0965d234f40bb935aef6360bc5418ac51da9a
anchor-sha256: contracts/target-machine/id-and-fingerprint-policy.toml df51b24d5ff63004c388dfd7621037d44c20b45ccff29a195680f715b5b7c5e2
anchor-sha256: scripts/target_machine_phase0.py 16c143459d0486a2d25f802a2f1906071d62a5046c52eaf544de6c0d53c0678c
anchor-sha256: scripts/run_target_machine_matrix_row.py d0bc4d973e9d6276079f92d3bcddb0aacd5e7a09c4bdfe043b89e0700027ce36
anchor-sha256: scripts/check_legacy_product_residue.py c3f15f8812355cb1bd3b316137d5cabc08ad2d915a1c92431c018923842bc327
anchor-sha256: scripts/check_runtime_header_dependencies.py 917a82b6ecff974005edfad476b18ed5653be50ccc75952451de5ec2ae9afdd2
anchor-sha256: tests/target-machine/test_matrix_row_runner.py 1cda4e1b05172f0bbeddf2036d4c3f87e53ccf94b03f6f2c162b2daa5972dc9e
anchor-sha256: tests/target-machine/phase0/run_baseline.py 0526f2537b8619ddf28b6cba6e37d99bb9e96785ad021c3413465bb34e4a79c5
anchor-sha256: tests/benchmarks/target-machine/source_run/main.xr be3a3a1f6a22d53958ff113d04f721549b9c9dde53f1f29ad52a645823b7a752
anchor-sha256: tests/benchmarks/target-machine/source_run/helper.xr 3ba2cbb299eea9802791671a726b51e6eea2f2e82b2492b727375f5d440a4ea5
anchor-sha256: tests/benchmarks/target-machine/source_run/helper.edited.txt 5e8b2152e52314bf013be19e4c6ebf39051b3dd254301a6df793e9a867d33359
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr fcf0b1a2b3aada657b47dc96ae89e0fef7b511fe66e6cc23bb90d448328bb998
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/manifest.toml c350e756a3125593e0c89f36f0cb6363bf826b46e3ed0edfbb7426834cdc4837
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/report.json 2f1dd9519207a52797a522c4de4b72051a323af710da53c6c41726a05c56e588
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/mailbox.xr 27c33d6af10868857d964c6429848f39c183f1650713e6575f6014e01b416dd0
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/manifest.toml 135954b1e3d0e8fec591f04add8f52de869de2c06cba60e0832d2e006391c9ab
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/report.json 240abb288cd49a4522e2a3a84785730bce8f3f76f5da7516c13b0677d0093d51
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/workload.xr ec1c60e3793dabe0ca889a1489c728ce077bdf5c026bd83c2d392839975df558
anchor-sha256: tests/target-machine/phase0/negative/manifest.toml af0f73be35c7021e07e0473bf080706838f7fbb0d4e147e5cb9e1bc54b98eec4
