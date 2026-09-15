# Runtime ABI descriptor and extent foundation contract

Status: preparatory runtime leaf frozen by task 274.

This contract freezes the target-plan-independent representation consumed by a
future canonical runtime ABI cutover. The canonical five-field object header is
materialized by this leaf, but no value family, VM, or AOT allocation path has
switched to it yet.

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
   `object_kind_id` is the stable identity of a canonical materialized carrier
   kind, never a private VM/AOT enum or an open-ended generic fallback.
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

TargetProfile runtime fingerprints and String-literal materialization facts
are derived from structured contracts. A nonzero byte string is not evidence.
String contract schema 2 embeds a headerless literal-view contract with the
exact dynamic tag, literal flag, five ordered fields, static-borrow semantic
ownership, and static-data backend ownership. The runtime canonical builder
and independent verifier both prove that structure before TargetProfile schema
2 copies it. This authority covers only immutable String literals; it grants no
general owned-String object body, allocation, root map, or cleanup path. Raw
digests and caller-authored field layouts are never accepted as substitutes.

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

`XrRuntimeObjectHeader` is the materialized implementation of this contract:

```c
typedef struct XrRuntimeObjectHeader {
    _Atomic int32_t rc;
    uint16_t object_kind;
    uint16_t flags;
    uint32_t layout_id;
    uint32_t domain_id;
} XrRuntimeObjectHeader;
```

Its hard budget is 16 bytes with four-byte alignment and offsets 0/4/6/8/12.
Static assertions reject compiler layout drift. The initial RC is positive one;
retain/release deltas are +1/-1; `INT32_MIN` is the sticky sentinel and values
through `INT32_MIN + 1024` form the sticky band. Local access is plain and
shared access uses the frozen relaxed/release/acquire protocol.

The v1 registry contains only the carrier families specified by design 910:
string, closure, boxed aggregate, array, map, set, instance, boxed enum, and
cell. Tuple materialization folds into boxed aggregate and non-owning views do
not receive headers. There is no generic/opaque entry. Concrete behavior comes
from the verified layout table. Stable IDs are fixed literals derived under
`contracts/target-machine/id-and-fingerprint-policy.toml` from canonical keys
under `xray.runtime.object-kind.v1/`; a governance KAT independently recomputes
every literal with the policy's v1 domain and u32 framing and checks collisions.
The flags registry is empty in schema v1 and all 16 bits are reserved zero;
destructor and domain facts remain in their verified tables instead of becoming
unchecked duplicate header state.

The materializer consumes `XrRuntimeObjectHeaderMaterializationFacts`, not
implicit host layout. These pointer-free facts record exact scalar/atomic size
and alignment, all five offsets, target endian, two's-complement encoding,
lock-free atomic-i32 RMW support, required relaxed/acquire/release orders, and
reserved-zero fields. Compiler code supplies facts from a target provider
probe. Runtime code independently creates native facts from the compiled type.
Any fact other than the exact 16-byte/4-aligned canonical ABI fails closed; the
materializer self-validates the complete `XrRuntimeObjectHeaderAbi` and
publishes no output on failure. Header initialization rejects unknown kinds,
reserved flag bits, and invalid layout/domain table indexes.

The planner's entity-ID implementation is not a production dependency of this
leaf. TargetProfile wiring must align it with the frozen v1 policy before a
layout descriptor can name these kinds; no v2-ID compatibility map is allowed.

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
XrRuntimeAbiStatus xr_target_provider_call_abi_fingerprint(
    const XrTargetProviderCallAbiContract *abi,
    XrFingerprint *out);

XrRuntimeAbiStatus xr_target_provider_set_fingerprint(
    const XrTargetProviderContract *providers,
    size_t provider_count,
    uint64_t *out_provider_capabilities,
    XrFingerprint *out);
```

`XrTargetProviderCallAbiContract` is a pointer-free canonical physical call
schema, not a digest supplied by a manifest author. It records the exact C
calling convention, target endian, pointer width/alignment, a non-variadic
result slot, and at most eight parameter slots. Every slot records its value
kind, width, alignment, ownership direction, nullability, and pointee
constness. Version one accepts void results, fixed signed/unsigned integers,
IEEE binary32/binary64, data addresses, and code addresses. A fixed integer
parameter may be marked consumed when it is the exact resource token; results
and floating-point parameters cannot claim consumed ownership. Unsupported
aggregate or variadic provider calls fail closed until a later schema owns
their exact classification. Unused slots and all reserved fields are zero.
The builder validates the complete schema before publishing a derived digest;
failure leaves the output unchanged.

Each pointer-free provider contract contains the runtime profile, special
runtime role, stable contract ID, ABI schema, flags, and a stable-ID-sorted
operation table. Operations carry their complete call ABI and explicit
physical effect/lifetime/failure facts. Allocator and panic roles additionally
validate their allocation and panic policy facts. Ordinary operation sets
carry neither special role's policy fields.

The provider set is strictly sorted by the complete contract ID and rejects
duplicate IDs. Multiple ordinary contracts may coexist, including contracts
with identical operation IDs in different contract namespaces. Exactly one
validated allocator and one validated panic contract own the foundation roles;
multiple contracts claiming either role are ambiguous and rejected.

`XrTargetRuntimeProfile`, `XrTargetProviderRole`, and the independent semantic
capability namespace are owned by `xr_target_runtime_profile.h`. Service names
such as clock, IO, random, or FFI have no role enum or capability bit. Consumers
select ordinary services by complete contract and operation identity. Provider
capabilities are derived from the verified records, never supplied as an
independent assertion. Allocator and panic capabilities require their full
role facts; unwind additionally requires the unwind policy. Output-write and
assertion-report require the exact IO contract ID, distinct operation ID, and
complete byte-sink ABI/effect/lifetime/failure facts. Copying a canonical
operation into another contract grants neither capability. Typed-error capture
is an execution capability and is never synthesized by a provider role.

Freeze rejects unknown roles, zero identities, invalid call slots, lifetime
summaries that disagree with slot ownership, and missing foundation contracts.
Failure publishes neither capabilities nor a fingerprint. Fingerprints encode
only verified structured facts; function addresses, vtables, discovery order,
library paths, link symbols, compiler objects, and file-content hashes remain
forbidden inputs.

`xr_target_profile_freeze` must invoke these structured builders internally and
store their results. It must not accept caller-authored raw
fingerprints. Until canonical header/value/provider registries and matching
runtime materialization exist, a production TargetPlan is incomplete and must
fail closed rather than choose an alias, compatibility path, or scalar
fallback.

## Verification

verification-test: test_runtime_descriptor
verification-test: test_runtime_abi_contract
verification-test: test_runtime_object_header
verification-test: test_runtime_string_object
verification-test: test_target_profile_authority
