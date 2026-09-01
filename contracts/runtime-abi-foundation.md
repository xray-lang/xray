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
    uint64_t *out_provider_mask,
    XrFingerprint *out);
```

`XrTargetProviderCallAbiContract` is a pointer-free canonical physical call
schema, not a digest supplied by a manifest author. It records the exact C
calling convention, target endian, pointer width/alignment, a non-variadic
result slot, and at most eight parameter slots. Every slot records its value
kind, width, alignment, ownership direction, nullability, and pointee
constness. Version one accepts void results, fixed signed/unsigned integers,
IEEE binary32/binary64, data addresses, and code addresses. Unsupported
aggregate or variadic provider calls fail closed until a later schema owns
their exact classification. Unused slots and all reserved fields are zero.
The builder validates the complete schema before publishing a derived digest;
failure leaves the output unchanged.

Each pointer-free provider contract contains the runtime profile, provider
kind, stable provider-contract ID, ABI schema, flags, and a stable-ID-sorted
operation table. Each operation contains its stable ID, the complete canonical
call-ABI structure, and effects/lifetime/failure flags,
and kind-specific facts such as allocator alignment/sized-free/zeroing/thread
safety or panic unwind/no-return behavior. Provider kinds are strictly
increasing and unique; the mask is derived from the records, never supplied as
an independent assertion. Freeze rejects unknown kinds, zero identities,
duplicate operations, invalid call slots, lifetime summaries that disagree
with slot ownership, and missing hosted/freestanding mandatory providers. The
provider-set fingerprint serializes only verified structured call facts; it
never accepts a caller-authored operation digest. Function addresses, vtables,
discovery order, library paths, link symbols, compiler objects, target strings,
and file-content hashes are forbidden.

`XrTargetRuntimeProfile` and `XrTargetProviderKind` are owned only by
`xr_target_runtime_profile.h`. Their numeric values are the artifact/runtime
namespace itself: the random provider is kind 4, scheduler is 5, I/O is 6, TLS
is 7, and FFI is 8. Planner or artifact headers include this owner and must not
redeclare, alias, or numerically remap the enums.

`xr_target_profile_freeze` must invoke these structured builders internally and
store their results. It must not accept caller-authored raw
fingerprints. Until canonical header/value/provider registries and matching
runtime materialization exist, a production TargetPlan is incomplete and must
fail closed rather than choose an alias, compatibility path, or scalar
fallback.

## Digest anchors

anchor-sha256: src/base/xstable_id.h 3a7abe4d53ba0771a8b064e5d7c395d883253a1a9c65cc46a284872f7119c3b1
anchor-sha256: contracts/target-machine/id-and-fingerprint-policy.toml df51b24d5ff63004c388dfd7621037d44c20b45ccff29a195680f715b5b7c5e2
anchor-sha256: src/plan/semantic/xr_semantic_ids.h 86c48ef09925169c2a5ef4b1da71175285708cc3d2cb51c7b2163b99b43627d9
anchor-sha256: src/runtime/abi/xr_runtime_descriptor.h 09adf0b12a0e6a0299ecb01a83957807d683bf54ccf3d691e8e30f5d73adff5c
anchor-sha256: src/runtime/abi/xr_runtime_descriptor.c 1d755ca9d0bf8d830273dbaad86c2390ed74c35464913aba6127e0a1c9b4e423
anchor-sha256: src/runtime/abi/xr_target_runtime_profile.h 4201e94c19f0cf8c17bd85a1590b217621544c2a2a4745e49fe7098f94f3e954
anchor-sha256: src/runtime/abi/xr_runtime_contract.h b786851747d2808668f714e668a7ff7a2c325d8a704e9adfea342ed2770baf0c
anchor-sha256: src/runtime/abi/xr_runtime_contract.c abcdaf535396af094227cdaf2348d88760396d61ecdf8d2fa9f776d61b7edee7
anchor-sha256: src/runtime/abi/xr_runtime_object_header.h fd04f1ca2c71e3b3b9682bf1a7b1e6ff6fe1af4bacea8f49e3f5b4087d6ee51e
anchor-sha256: src/runtime/abi/xr_runtime_object_header.c 59fbac2c2fd4a195f2be2980217036636db1fdeadcd05993e5f6e528bfbbf307
anchor-sha256: contracts/target-machine/runtime-string-object-contract.toml d3304d0e964364eac065c67b3eb373e3267bb42eab9c1003d558a2e27d9adee6
anchor-sha256: src/runtime/abi/xr_runtime_string_object.h 449864cf27dde72d9e063ceb824684e1089e241742f1ccba1677b34b9726f2a1
anchor-sha256: src/runtime/abi/xr_runtime_string_object.c 5b5b658ea9afe0abede35c8ac4779d09e79f5fe1a1dfefba01a7dd6ec6730f54
anchor-sha256: tests/unit/runtime/test_runtime_descriptor.c 76e3c93da9b9acc28d14fd83bc9d31504e54082ebf9349c517f3fac897487e46
anchor-sha256: tests/unit/runtime/test_runtime_abi_contract.c 6252f7fc4712596c39a4dbc42b4633dbb144f6b2638f67040ff7642cd8f7ee61
anchor-sha256: tests/unit/runtime/test_runtime_object_header.c 05f3c1bd1e157e010cdddac4fb827294c49ca08599f7b8d52f2490cc0efaea95
anchor-sha256: tests/unit/runtime/test_runtime_string_object.c 99d46076417b73f92632ec138e1bf9c57664d7d9bbd5564a3e7458cd34ecd632
anchor-sha256: tests/unit/CMakeLists.txt 220175a3ed455c54b598fdf4373d4bd22f5590fb86bf17f404da7578aeae23f0
anchor-sha256: scripts/check_runtime_object_header_boundary.py 66e28cadaf5c456eca44528ae8ccb0926089d4ef603e6c06ca77113ddd0a7282
anchor-sha256: scripts/check_runtime_string_object_boundary.py 81921b6f6b1118f99fc9d107b6c77d7611aec6f9fb11a11c5d80e8bf06c1de1d
