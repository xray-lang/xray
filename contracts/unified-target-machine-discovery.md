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

An obsolete private AOT plan is deleted rather than renamed or copied.  Json
code generation consumes the verified `XgJsonCodecSummary` semantic evidence
directly; the destination inventory therefore records
`delete-private-plan-consume-semantic-evidence-directly` as the obsolete-row
policy instead of implying that the semantic evidence gate is removed.

The support matrix distinguishes supported, CI-only, unqualified, and
unsupported configurations.  A failed row cannot be relabelled as a skipped
configuration.

Semantic-owner discovery is target-aware. Each Xi row derives VM and AOT
applicability from `ops.def` and resolves executable ownership through the
canonical `lowering.def` implementation kind. Generated expression and
statement drivers retain their exact generated dispatch symbols; direct
consumers project every terminal path and symbol, while router, selector,
guarded-selector, structural, and structured-loop bypass sources remain
fingerprinted witnesses rather than extra owners. The generator ratchets 33
direct-consumer operations, 44 terminal bindings, six routers, 36 exact
terminal-emitter witnesses, six explicit selector-predicate witnesses, two
explicit predicate-domain witnesses, one
guarded selector, 13 activation-edge records
covering 15 exact calls, three exact
output sequences, and 11 statement-driver rows, and rejects stale generated
coverage or dispatch. A Xi row records its executable VM and AOT lowering owner
independently of `current_shared_owner`, which preserves its canonical semantic
kernel. Each `shared.*` kernel row derives applicability only from its profiles;
an applicable shared-kernel leg records exactly `representation adapter`, while
an inapplicable leg is JSON `null`. Generic per-operation executor ownership
remains visible as migration residue and must not be mislabeled as a canonical
source-backed adapter. Phase 0 lowering-first regeneration is the sole producer
and grammar owner for exact generated dispatch, direct-consumer source, and
terminal symbol identity. The migration and completion consumers compare the
entire checked-in inventory to that regenerated projection before applying
their own category or terminal-debt rules; neither keeps a second owner-string
grammar. Inventory schema 1 is a current-source JSON shape contract, not a
persistent compatibility format. Tightening a current owner value therefore
requires atomic regeneration and synchronization of every repository consumer,
not a compatibility reader or a second schema path.

The Xi-lowering build edge publishes only one build-tree validation stamp and
its generator-authored depfile. Ninja consumes the depfile and revalidates when
the stamp or a dynamic dependency is dirty; no second deep-validation scheduling
owner exists. The twelve checked-in projections remain ordinary tracked source
inputs, never generated build outputs. An always-run, content-only verification
edge rejects a missing or noncanonical projection before dependent compilation,
without rewriting it or manufacturing downstream work through mtime churn. The
depfile records the immutable local-C include closure rooted at
`src/aot/xi_cgen.c`, which is the translation unit that compiles every
declared direct consumer, router, activation edge, and structural witness.
Every recursively reachable literal quoted `.c`, `.h`, and `.def` include is a
descriptor-bound dependency, so edits and deletions re-run
fail-closed validation, while a new include edge first changes an already
tracked parent and then extends the next validated closure. Quoted includes use
the exact compiler search order: the including file's directory first, followed
by the governed `XRAY_COMMON_INCLUDES` directories. Every existing search
directory and its stable entry listing participates in the stamp, depfile,
recapture equality, and closure fingerprint, so a new earlier match cannot leave
the prior resolution valid. This validation domain is deliberately restricted to
repository-local quoted includes and repository directories from that ordered
list. The build-tree generated include directory is outside the repository and
therefore cannot resolve a quoted closure member; such a resolution fails closed
instead of broadening the governed source domain. A separate recursive
`src/aot/**/*.c` discovery snapshot examines regular, non-symlink source bodies;
every discovered source and traversed directory is a depfile dependency so
additions, deletions, and content changes are reconsidered. These are discovery
dependencies, not semantic owners. That census may only reject an undeclared selector, router,
guarded selector, activation edge, or structural owner matching canonical Xi
behavior. A plain unrelated C file is not semantic authority, and
`lowering.def` remains the only declaration table. Lexical include paths reject
backslashes and parent steps after a child component. Leading parent steps are
accepted only while the selected compiler search root remains inside the
repository. Snapshot reads do not
perform a pathname check followed by a pathname reopen: POSIX traverses from an
anchored repository descriptor with no-follow component opens and verifies each
opened object with `fstat`; Windows holds non-reparse-point component handles
without write/delete sharing through the read. Deterministic file and directory
swap mutations at the former check/open boundary therefore fail closed, and
`symlink/..` can never erase a compiler-visible traversal.
The scanner applies trigraph replacement, backslash-newline splicing, and C
comment removal before recognizing both `#` and `%:` include directives, so a
directive token cannot hide traversal across physical lines or comments. It
accepts all non-newline C directive whitespace and the compiled literal
local-include spelling even when a legal sequence of trailing comments is
present. Macro-expanded include operands are forbidden and fail closed rather
than creating a second preprocessor or an untracked include path. The validator
removes the old output, atomically
publishes the depfile and final proof stamp, then recaptures the schema inputs,
compile closure, and discovery snapshot. The proof contains exact ops,
lowering, closure, and discovery digests plus their counts; any mismatch or
exception removes both published artifacts and fails. Ninja consumes the
validator's depfile on the same build-tree stamp edge, so no projection is
declared as generated and no second deep-validation owner exists. The `xisa/xi`
parent directory is also a dependency, so normal Ninja mtime tracking observes
atomic schema replacement as well as file content changes. No stale or
mismatched proof can become an accepted validation edge. An uncatchable
termination after durable publication remains a failed build edge; an unchanged
retry reruns validation before any dependent command. Ninja's failed-edge
bookkeeping is the external durable acknowledgement: a production-shaped
dependency-chain mutation kills validation after proof publication, proves that
the ordered content check cannot run, then proves that an unchanged retry reruns
validation before the check succeeds. This freezes the recovery boundary without
inventing a second completion marker. All twelve checked-in lowering projections,
including the generated C unit test, are ordinary tracked inputs. The always-run
fast edge derives their canonical bytes from the schemas and rejects drift; it
never rewrites a projection, and Ninja clean cannot delete one.

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
anchor-sha256: contracts/target-machine/semantic-owner-inventory.json a5c2a4b2cffd3fa3775c0ec2fcadc9fda1e3615ff00d772a75c92a2ce4b50737
anchor-sha256: contracts/target-machine/aot-plan-destination-inventory.json 57a27ff4326ad81a88fa6d16b81118f86e831ad4d1abaf2065f79303c4ad20ee
anchor-sha256: contracts/target-machine/legacy-vm-inventory.json a1b272985a73b80df6eab458fff3a221f08af8c32488ee199cb48633552c1916
anchor-sha256: contracts/target-machine/legacy-product-residue.json c335bd1360bdbd242d642a4ef5990072a2111345daf237e87cb4af103967f230
anchor-sha256: contracts/target-machine/migration-source-classification.json e0adb045a60f4b5d4c5a5f243b6a43cb217236190a4d2163eab2368f8663cacc
anchor-sha256: scripts/check_target_machine_migration_classification.py f3c363ad2bfcd0341b9e54f606f74c33bdd52cc26608995759f90574dbeb98e6
anchor-sha256: scripts/target_machine_retired_runtime_symbols.py 3db52d4670d4d76a640d91709f5a6fdd091511ac421ca6326c34ed3b8739d4f7
anchor-sha256: contracts/target-machine/object-extent-inventory.json ac663ab720c82c56f8c40be563489dd3fd835376750b673f1e1df7dfc734786c
anchor-sha256: contracts/target-machine/validation-matrix.json e6a1610c21edc6bfb4d7f92a535f3680e0952c57e4e835d51778edc3bbe7ef74
anchor-sha256: contracts/target-machine/baseline-manifest.json f6af567e8dc686ae76733bc0b2b7cbc6d2a078d1730af6434dd35aed338dfb05
anchor-sha256: contracts/target-machine/diagnostic-codes.toml c939590da4fe46af9f42774997c322b6c968e2c1b5621541f540ee40ec4f28b7
anchor-sha256: contracts/target-machine/id-and-fingerprint-policy.toml df51b24d5ff63004c388dfd7621037d44c20b45ccff29a195680f715b5b7c5e2
anchor-sha256: scripts/target_machine_phase0.py d394ef70549a3267cef631dbacf3ecbb18ffb035b56a0dcc46dc71c8d7aecec2
anchor-sha256: scripts/run_target_machine_matrix_row.py 2a480a236d2bc486a9eea12429b52d10b38a71c637e86712e8660ccb1a5f08b3
anchor-sha256: scripts/check_legacy_product_residue.py 0d8b95a014d23f7732e46b837f8c8d1cda3406da1464b314e6d2f401bd2a3705
anchor-sha256: scripts/check_runtime_header_dependencies.py aaa81090a84a890c9e265de999b3eed00e303dbb8019f04385bb91556fe4f399
anchor-sha256: tests/target-machine/test_matrix_row_runner.py 1cda4e1b05172f0bbeddf2036d4c3f87e53ccf94b03f6f2c162b2daa5972dc9e
anchor-sha256: tests/target-machine/phase0/run_baseline.py 0526f2537b8619ddf28b6cba6e37d99bb9e96785ad021c3413465bb34e4a79c5
anchor-sha256: tests/benchmarks/target-machine/source_run/main.xr be3a3a1f6a22d53958ff113d04f721549b9c9dde53f1f29ad52a645823b7a752
anchor-sha256: tests/benchmarks/target-machine/source_run/helper.xr 3ba2cbb299eea9802791671a726b51e6eea2f2e82b2492b727375f5d440a4ea5
anchor-sha256: tests/benchmarks/target-machine/source_run/helper.edited.txt 5e8b2152e52314bf013be19e4c6ebf39051b3dd254301a6df793e9a867d33359
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/loop_suspend_try_catch.xr fcf0b1a2b3aada657b47dc96ae89e0fef7b511fe66e6cc23bb90d448328bb998
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/manifest.toml c350e756a3125593e0c89f36f0cb6363bf826b46e3ed0edfbb7426834cdc4837
anchor-sha256: tests/target-machine/phase0/coroutine_vertical/report.json 2f1dd9519207a52797a522c4de4b72051a323af710da53c6c41726a05c56e588
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/mailbox.xr 9e88b914cd7e877b8e67ca90c7a3f1ae8b1e6c887cb6099730aae5aecf9c46a6
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/manifest.toml 135954b1e3d0e8fec591f04add8f52de869de2c06cba60e0832d2e006391c9ab
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/report.json 240abb288cd49a4522e2a3a84785730bce8f3f76f5da7516c13b0677d0093d51
anchor-sha256: tests/target-machine/phase0/typed_slot_calibration/workload.xr ec1c60e3793dabe0ca889a1489c728ce077bdf5c026bd83c2d392839975df558
anchor-sha256: tests/target-machine/phase0/negative/manifest.toml af0f73be35c7021e07e0473bf080706838f7fbb0d4e147e5cb9e1bc54b98eec4
