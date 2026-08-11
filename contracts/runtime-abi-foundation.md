# Runtime ABI descriptor and extent foundation contract

Status: preparatory runtime leaf frozen by task 274.

This contract freezes the target-plan-independent representation consumed by a
future canonical runtime ABI cutover. It does not claim that object headers,
value families, VM, or AOT materialization have already switched.

1. Stable IDs and fingerprints are pointer-free byte value types shared across
   compiler, plan, runtime, and audit layers. Runtime code does not depend on
   analyzer, Xi, planner, or C generator data structures.
2. One layout/extent pair describes one physical allocation. Every external or
   multi-buffer backing allocation therefore has its own pair. A single
   descriptor cannot prove group completeness; the runtime group verifier
   validates a complete set for dense part indexes, unique extent/descriptor
   identities, one group identity, and a canonical order-independent summary.
3. Layout uniquely owns fixed prefix size and memory alignment. Extent uniquely
   owns tail offset, stride, operand, provider, and group/part facts. Evaluated
   bytes are derived data and retain the source extent ID and fingerprint.
4. `descriptor_id` names the immutable runtime descriptor. `layout_id` names
   the verified source layout-plan identity from which it was generated.
   `object_kind_id` is a stable identity, not a prematurely frozen runtime tag
   registry or a private VM/AOT enum.
5. Semantic ownership domain and backend materialization remain independent
   axes from `xstorage.h`. An actual domain additionally has a stable contract
   ID and nonzero runtime instance ID. Category allowlists never erase exact
   identity equality.
6. Fingerprints use `xray-runtime-abi-v1\0`, a record discriminant, fixed-width
   little-endian integers, and field bytes in declaration-independent order.
   C struct bytes and padding are never hashed or serialized. Layout
   fingerprints bind the referenced extent fingerprint, not only its ID.
7. Fixed, inline-tail, external, multi-buffer, and provider-defined formulas
   reject malformed shapes, missing operands/providers, overflow, alignment
   overflow, and configured allocation/alignment limits. A null operand vector
   with a nonzero count is rejected before provider dispatch. There is no
   legacy formula fallback.

## TargetProfile fingerprint handoff

The three TargetProfile runtime fingerprints are derived from structured
contracts. A nonzero byte string is not evidence. The builders in this leaf
validate pointer-free candidate schemas without treating the current legacy
`XrObjHeader` as an input. Production profile freeze remains blocked until the
five-field header and canonical dynamic-value registry are materialized in the
runtime and supplied to these builders; this prevents `objsize`, the overloaded
auxiliary word, and backend-private flags from entering the new ABI.

The required object-header entry point is:

```c
XrRuntimeAbiStatus xr_runtime_object_header_abi_fingerprint(
    const XrRuntimeObjectHeaderAbi *abi,
    XrFingerprint *out);
```

`XrRuntimeObjectHeaderAbi` is pointer-free and contains an exact schema version,
target endian, total size/alignment, a padding-must-be-zero rule, and exactly
five ordered field contracts for `rc`, `object_kind`, `flags`, `layout_id`, and
`domain_id`.
Each field contract contains its semantic role, offset, width, alignment, and
signed/unsigned/atomic encoding. The header contract additionally contains:

- local/shared RC polarity and retain/release deltas, the initial value, and
  the sticky comparison, sentinel, and band boundary;
- object-kind encoding width, invalid value, and a complete stable-kind-ID to
  numeric-encoding table;
- flag encoding width, valid mask, reserved-zero mask, and a complete stable
  flag-ID/bit/exclusivity-group table;
- layout/domain ID width, invalid sentinel, and the verified-table-index
  semantic rule.

The fingerprint function rejects missing or overlapping fields, misalignment,
out-of-range encodings, duplicate IDs/values/bits, unsorted registries,
inconsistent masks, nonzero reserved fields, and unknown policy enums before
hashing. Registry rows are strictly ordered by stable ID; discovery order,
names, target strings, addresses, paths, and C struct bytes are never inputs.
The canonical header contract must not contain `objsize`, VM/AOT-private tags,
weak/cycle slots, or a reusable auxiliary word; evaluated extent and allocator
metadata own those facts.

The required whole-runtime entry point is:

```c
XrRuntimeAbiStatus xr_runtime_abi_contract_fingerprint(
    const XrRuntimeAbiContract *abi,
    XrFingerprint *out);
```

`XrRuntimeAbiContract` contains its schema, canonical numeric serialization
endian, stable-ID/fingerprint/pointer widths, the verified object-header
contract, and the canonical dynamic-value contract. The dynamic-value contract records
size/alignment, every tag/flags/payload field offset and width, null/object-ref
encoding, and the complete stable tag registry. It also contains exact
physical field contracts and enum/sentinel namespaces for domain identity,
extent descriptor, layout descriptor, extent limits, evaluated extent, and
the persisted extent-group summary. The provider-defined extent callback
contract records the provider ID, operand element/count/result widths, return
status namespace, and error-normalization rule. Checked arithmetic, alignment,
unknown-enum rejection, and reserved-zero policies are explicit versioned
fields. The pointer-bearing `XrRuntimeExtentGroupEntry` verifier view is
ephemeral and is therefore excluded. Variable tables use fixed-capacity inline
storage plus validated counts. Counts above the frozen budgets fail before an
element is read, and every unused slot must be all-zero through its declared
members.

Provider selection remains a separate fingerprint boundary:

```c
XrRuntimeAbiStatus xr_target_provider_set_fingerprint(
    const XrTargetProviderContract *providers,
    size_t provider_count,
    uint64_t *out_provider_mask,
    XrFingerprint *out);
```

Each pointer-free provider contract contains the runtime profile, provider
kind, stable provider-contract ID, ABI schema, flags, and a stable-ID-sorted
operation table. Each operation
contains its stable ID, call-ABI fingerprint, effects/lifetime/failure flags,
and kind-specific facts such as allocator alignment/sized-free/zeroing/thread
safety or panic unwind/no-return behavior. Provider kinds are strictly
increasing and unique; the mask is derived from the records, never supplied as
an independent assertion. Freeze rejects unknown kinds, zero identities,
duplicate operations, zero call ABI, and missing hosted/freestanding mandatory
providers. Function addresses, vtables, discovery order, library paths, link
symbols, compiler objects, target strings, and file-content hashes are
forbidden.

`xr_target_profile_freeze` must invoke these structured builders internally and
store their results. It must not accept caller-authored raw
fingerprints. Until canonical header/value/provider registries and matching
runtime materialization exist, a production TargetPlan is incomplete and must
fail closed rather than choose an alias, compatibility path, or scalar
fallback.

## Digest anchors

anchor-sha256: src/base/xstable_id.h 3a7abe4d53ba0771a8b064e5d7c395d883253a1a9c65cc46a284872f7119c3b1
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 7ec819570b47e2a3f01132fc729eb73f91dda65cf2d343cb9bee34ad229b4284
anchor-sha256: src/runtime/abi/xr_runtime_descriptor.h ce4f02055de60da60e1e36e04f522508fc0259ada826b29bc39c4bcb9ee0478b
anchor-sha256: src/runtime/abi/xr_runtime_descriptor.c 1d755ca9d0bf8d830273dbaad86c2390ed74c35464913aba6127e0a1c9b4e423
anchor-sha256: src/runtime/abi/xr_runtime_contract.h aae1d9df44c1635c09cd2db86583a9695f5d324ce697cd45d483f7546505ba1a
anchor-sha256: src/runtime/abi/xr_runtime_contract.c 9d6c6c1587a632131b2cae3e0aa41501709cc54660b7ee0673e697ddd63a9b2c
anchor-sha256: tests/unit/runtime/test_runtime_descriptor.c 76e3c93da9b9acc28d14fd83bc9d31504e54082ebf9349c517f3fac897487e46
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 57c30a96995f4d18dc955d53035dd7f7fe61d3be93d2373f55be142a0dbc8ddb
