# Backend-neutral semantic ownership contract

Every observable operation in the generated `semantic-owner-registry.json`
has exactly one canonical semantic owner, a stable 128-bit operation ID, a
stable 128-bit owner ID, and at least one declared production consumer. The
registry is generated from `xisa/xi/ops.def`; it is not a second planner or a
backend-owned name table. Unknown, unowned, or multiply owned operations,
duplicate stable IDs, missing consumer bindings, and stale fingerprints fail
closed during generation or contract verification.

Native and tagged implementations may use different representations, but
adapters may only marshal representation, ownership, errors, and ABI state.
They must not duplicate observable rules. VM, AOT, CGen, and runtime bindings
for a migrated shared owner consume its generated stable owner ID. CGen adapter
spelling is obtained by stable-ID lookup rather than reconstructed from an Xi
or source-language name.

`xi.regex.compile` binds its observable contract directly to
`stdlib/regex/regex.xr`, whose `stdlib.regex.compile-match` identity is the sole
compile/match owner. The VM and runtime prove exact membership in that owner;
the AOT target is inapplicable and has no adapter. `xr_regex_core.h` is a
separate `regex-flag-contract` utility for flag spelling and scalar boundaries,
not a regex engine or an alternate compile/match owner. A missing VM/runtime
binding, an AOT compile/match adapter, or owner logic in the flag utility fails
closed.

Bounds semantics have one real operation family: canonical `xi.index.get` and
`xi.index.set`, whose generated VM and AOT consumers perform the access. The
retired `xi.bounds.check` row had no source-lowering producer and no executor
handler, so it is absent from the owner registry and target-machine inventory;
no alias, opcode hole, or alternate bounds owner remains.

The generated shared-core inventory records signatures, production callers,
test callers, representation class, and applicable profiles for every
`src/shared/xr_*_core.h`. A new or removed core, caller drift, a native kernel
that gains `XrValue`, loss of either production `Array.sort` consumer, or
revival of a retired private sort implementation fails the gate.

`shared.truthiness` is the first production owner migrated through this
registry. Its representation adapters classify null, bool, integer, float,
sized, and opaque-object values; the observable decision remains exclusively
in `xr_truthy_core_eval`. Removing a declared production binding, changing its
stable ID marker, or reviving a private truthiness decision fails the owner
ratchet.

`shared.assertion` owns the typed `xi.assertion` plan, action-channel
classification, failure schema, renderer, and target bindings. VM and AOT
consume that plan directly; retired condition/equality opcodes and source-name
switches are not semantic fallbacks.

`primitive.type-identity` owns canonical `xi.typeid`. VM, runtime, hosted AOT,
freestanding AOT, and CGen consume the generated owner ID and declared consumer
bit; representation adapters only classify local value tags into the shared
public TypeId core. CGen resolves `xrt_typeof_id` from the generated owner table,
and the legacy `typeOf` source-name dispatch has no remaining production path.
The generated registry, low-level owner header, SemanticPlan row, and
machine-readable fingerprint are all derived from the same `ops.def` row.
Restoring a source-name/literal adapter guess, borrowing another consumer bit,
or losing any of the six production bindings fails the owner ratchet.

`shared.range` owns the start, end, step, boundary mode, length, membership,
index, and display rules for `xi.range`. VM and AOT runtime code only allocate
and expose their local Range representation; CGen resolves
`xrt_range_semantics` by stable owner ID before calling that allocation
adapter. Negative indexing is rejected, inclusive and half-open boundaries are
decided in the shared core, and the INT64 edges are covered by the owner KAT.
Restoring backend-local Range formulas, raw construction helpers, or a literal
CGen adapter binding fails the owner ratchet.

`shared.bitwise-binary` owns `xi.band`, `xi.bor`, and `xi.bxor` for signed
64-bit patterns and arbitrary-precision BigInts. BigInt sign-magnitude storage
is converted according to unbounded two's-complement rules inside the shared
kernel; result sign and normalization are owner decisions. VM, optimizer,
hosted and freestanding AOT, CGen, and runtime code only adapt tags, allocation,
errors, and emitted representation. Per-operation BigInt helpers, operator
overload fallback, raw optimizer folds, or raw CGen `&`, `|`, and `^` emission
fail the owner ratchet.

`shared.shift` owns `xi.shl` and `xi.shr` for scalar and BigInt values. Scalar
counts use modulo 64, left shift wraps, and right shift is explicitly signed or
unsigned. BigInt counts are validated without modulo reduction and limb
planning/application remains in the same owner. VM, optimizer, hosted and
freestanding AOT, CGen, and runtime code may only adapt representation,
allocation, and errors; reviving private shift helpers or raw CGen shift
emission fails the owner ratchet.

`shared.numeric-neg` owns `xi.neg` across signed 64-bit integers, IEEE-754
floats, and normalized BigInts. Integer negation wraps at `INT64_MIN`; floating
negation toggles only the sign bit and therefore preserves signed zero and NaN
payloads; BigInt zero normalization and sign inversion come from the shared
plan. VM, optimizer, hosted and freestanding AOT, CGen, and runtime code may
only adapt value representation, allocation, and errors. Operator overloads,
raw C unary negation, private constant folding, and backend-local BigInt sign
rules are forbidden by the owner ratchet.

`shared.codegen-compiler-fence` owns the compiler-order edge of
`xi.codegen.compiler-fence`. The shared plan preserves program state, declares
no runtime memory effect, and requires a native provider to block memory
reordering across the boundary. VM bytecode projects this to a void value
because no native optimizer exists there; CGen emits the selected provider's
compiler fence. Neither executor may reinterpret the fence as a runtime memory
operation, drop the native ordering edge, or bypass the stable owner ID and
exact plan check.

Higher-order array operations retain the existing native typed fast path for
pure uncaptured callbacks. Captured, dynamic, generic, cross-module, and tagged
forms follow the explicit matrix in `hof-shape-matrix.toml`; unknown target or
effect evidence cannot be guessed inside CGen.

`shared.target-layout-query` owns `xi.target.sizeof` and `xi.target.alignof`.
The owner evaluates a frozen `XrTargetDataLayout` plus the native scalar tag;
VM only materializes the result and CGen emits the already-decided integer.
Generated C may not ask the host compiler to rediscover target layout through
`sizeof` or `_Alignof`, and invalid layout, query, or type inputs fail closed.

## Digest anchors

anchor-sha256: contracts/semantic-owners.toml 05ddaa0d1870aac072fa918a9f444b521c55a305eff4509fd1c9172769980447
anchor-sha256: contracts/semantic-owner-registry.json 67930d92d51b636e044bee023dd7ea13c3dd60ced6c2221cc1f3bb44a437739d
anchor-sha256: contracts/hof-shape-matrix.toml e64c5c47454ee0ab56b28086cdded0dd7e962d89cc6bf72b37ba2677a715fbf7
anchor-sha256: contracts/shared-core-inventory.json 76b550c56235449de623e213d9afc7efb83d35145962a99a923a3661df7bedf8
anchor-sha256: src/shared/xr_semantic_owner_ids_gen.h aa0cca97cfab375867d60f94597feb9c5b75c58b3c3b8761ccd597e81ba4b5e6
anchor-sha256: scripts/check_semantic_owners.py b68ad3ae27d9010c07b7809e2319beb6aa057c88cb2273177dce68e80cc8116b
