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

anchor-sha256: contracts/target-machine/semantic-owner-inventory.json 0b7e445ee298f308423d42732230afd7647a93361c91a6b15e08fee31784bb95
anchor-sha256: contracts/target-machine/aot-plan-destination-inventory.json 30d79df402954c0fe4afe482524d5b831d5865d3637ae08ff9fbe8eb6e4cb38e
anchor-sha256: contracts/target-machine/legacy-vm-inventory.json aa463ab2250f7f76eadb197b9e949f1b51a1f988b7a8b425c0e282fb4d102bf6
anchor-sha256: contracts/target-machine/object-extent-inventory.json 9164a835db2c2793ce9374913bafc46039ccbb5e07a0abe0bee6b2660796cff0
anchor-sha256: contracts/target-machine/validation-matrix.json 983dd83a1acc22fc66576441dcdfa3b8b52ee606fb6256abefefa2e848310aed
anchor-sha256: contracts/target-machine/diagnostic-codes.toml 17cca3dcffa272a844d56726d970f4ccce57870ff6a5cd292be58705e75d6d97
anchor-sha256: contracts/target-machine/id-and-fingerprint-policy.toml df51b24d5ff63004c388dfd7621037d44c20b45ccff29a195680f715b5b7c5e2
anchor-sha256: scripts/target_machine_phase0.py 78891f53cec9cf3a5df7f411a49c106e0b9ef18017bfd4645095f50b8e0bc756
anchor-sha256: tests/target-machine/phase0/run_baseline.py ab23b59f1306f38eb8876f8906b840205923c1e57736f45f082fef5a880ce304
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr 40a77086074f6b47b0ea132b1700c62296b8341208c347066cc6e596082eb920
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/manifest.toml c350e756a3125593e0c89f36f0cb6363bf826b46e3ed0edfbb7426834cdc4837
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/report.json 2f1dd9519207a52797a522c4de4b72051a323af710da53c6c41726a05c56e588
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/mailbox.xr 84d14ec039196012c241ea9083ca86025e5b9e493c049f8a948c4a92f32a25cb
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/manifest.toml 135954b1e3d0e8fec591f04add8f52de869de2c06cba60e0832d2e006391c9ab
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/report.json 240abb288cd49a4522e2a3a84785730bce8f3f76f5da7516c13b0677d0093d51
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/workload.xr 29826802ad6370e69849e55576c68d1d4f90c9f485deca7fcc9615133116f987
anchor-sha256: tests/target-machine/phase0/negative/manifest.toml af0f73be35c7021e07e0473bf080706838f7fbb0d4e147e5cb9e1bc54b98eec4
