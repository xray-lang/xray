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
The three governed performance fixture files are explicitly checked out with
LF bytes on every host.  Their policy and contract digests therefore identify
the exact executed input instead of the caller's `core.autocrlf` preference.

anchor-sha256: .gitattributes e978a1bddfdafc2f706780f7bd9a0cca60ba71fb72fcb17a1ef782dacdd3d549
anchor-sha256: contracts/target-machine/semantic-owner-inventory.json d106f8c7a84b5d36f98836f9d7a114b4f62fc9a3dca47a67bacc45c9cda6089f
anchor-sha256: contracts/target-machine/aot-plan-destination-inventory.json f13bbef3b5e5f739e760d242847e53b431252ed719ce669d2c058c8dc806df17
anchor-sha256: contracts/target-machine/legacy-vm-inventory.json d9b7b9d3b40c27644adbf59c443aff6ed09d0726b03afee37cdb8bf7a9230add
anchor-sha256: contracts/target-machine/legacy-product-residue.json e3fd908fc1852f0d81743d444c1c7af12310930657c77c9cadfeec17d2765fd4
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: contracts/target-machine/object-extent-inventory.json f4d3941e9896c349b73e084556991a3dc06473fa2e91d9caedecd9127bb2d61c
anchor-sha256: contracts/target-machine/validation-matrix.json 983dd83a1acc22fc66576441dcdfa3b8b52ee606fb6256abefefa2e848310aed
anchor-sha256: contracts/target-machine/baseline-manifest.json 9264ec82b4e58d421c7b285e3bc2ff598ef09587622110de6cf72d79cbf7ddef
anchor-sha256: contracts/target-machine/diagnostic-codes.toml 54ee3affe064aabfb89fe1cc75ef5642837fd14b6e7e575a0badb4fc670efd74
anchor-sha256: contracts/target-machine/id-and-fingerprint-policy.toml df51b24d5ff63004c388dfd7621037d44c20b45ccff29a195680f715b5b7c5e2
anchor-sha256: scripts/target_machine_phase0.py 91de1b843c880cee07f59bf19864d5a0fea45f473936f9c2387eeac98b645e96
anchor-sha256: scripts/check_legacy_product_residue.py d160f8b9ab1d16da893bcc30a7ed90d583dda9e478dd11f67c9ce299629f8d2f
anchor-sha256: scripts/check_runtime_header_dependencies.py 917a82b6ecff974005edfad476b18ed5653be50ccc75952451de5ec2ae9afdd2
anchor-sha256: tests/target-machine/phase0/run_baseline.py d8ac40519305d9394b1857311eac49599a4532ab44226bc2f4323184047ce195
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
