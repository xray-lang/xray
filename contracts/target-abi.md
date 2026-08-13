# Cross-target ABI contract

Status: re-frozen after raw-pointer aggregate loads and ordinary slices were
classified as borrowed views, StringBuilder and Iterator storage became
ARC-managed, and implicit error cleanup was confined to the existing cold
propagation branch. Iterator values retain their traversed source without
changing their public value tag or body layout. The public target and Slice
ABIs are unchanged.
Portable SIMD values crossing hosted module shared slots recover their fixed
aggregate layout from the tagged reference; scalar, native, and cross-endian
lowering retain one lane-order contract.
Value-struct aggregates do the same: a shared slot always stores a boxed
XrValue, so a read planned as a native struct aggregate dereferences the payload
rather than assigning the box. A borrowed struct place parameter — a method
receiver included — keeps the native value ABI (`xrt_struct_abi_* `), recovering
its layout through the PLACE_LOAD its field ops read, and its whole-aggregate
temporary exists only under XRAY_AOT_DEBUG_LOCALS.
The name-keyed property store is the fallback for shapes that carry named
properties at run time — a Map and a JSON/record object — and it fails closed on
every other receiver. A store it cannot perform is a CGen gap, not a value to
discard: reporting it is the only alternative to a program that reads back a
value it was never given.
The same generated translation unit may now be compiled as GNU/Clang C++11,
but remains a C ABI artifact: exported definitions use C linkage, atomic fields
retain their scalar C layout and memory ordering through compiler builtins, and
typed pointer casts do not change the prepared target ABI. C11 remains the
default generated language and performance path.
An explicitly selected restricted-C90 translation unit is also a C ABI
artifact, but is confined to scalar, freestanding, shared-library core graphs
for LP64 Linux or Darwin targets. Its public scalar ABI uses C90 primitive
integer types guarded by width checks, and the build fails closed before host
compilation if the reachable graph needs ordinary runtime, standard-library,
native-input, aggregate-export, main-entry, or SIMD support. This opt-in lane
does not alter the default C11 or generated-C C++ target ABI.
Task 245 adds provider code-shape capability probes and typed adapters without
changing the selected target ABI: fallback may change the compiler provider,
never pointer width, calling convention, object format, runtime artifact, or
native target identity.
Task 253 adds per-function return-ownership metadata to analysis and Xi. The
metadata controls caller ARC placement only; it does not change the public
value representation, calling convention, parameter list, or native return
ABI.
Resolved import references additionally carry the callee function and owning
module they bind to, so ARC can read a cross-module callee's borrow signature
and keep caller-side ownership of an argument the callee only borrows. Like the
return-ownership metadata above, this controls caller ARC placement only: the
public value representation, calling convention, parameter list, and native
return ABI are unchanged, and an unresolved reference keeps the conservative
moved-argument convention.
Task 254 makes mutable capture cells explicit in Xi and changes the internal VM
cell opcode operand shape. Closure upvalues remain tagged `XrValue` slots and
the public target ABI, calling convention, and closure layout are unchanged.
Task 257 changes provider probe capture from implicit C text to bounded byte
buffers. Target triples are accepted only as strict ASCII; this changes no
target identity, generated-C ABI, calling convention, or runtime layout.
Task 256 replaces parallel VM/AOT value declarations with the versioned public
`xray_value_abi.h`, introduces `hosted_fragment` as a real artifact kind, and
generates the first scalar stdlib fragment from authoritative Xray source. The
fragment borrows the embedding runtime and exports only manifest-declared C ABI
entries; it cannot synthesize a program main, runtime implementation, or
constructor lifecycle.
Hosted Fragment ABI v7 adds a layout-neutral, call-scoped byte-span view for
`Array<byte>` and `Slice<byte>`. Generated fragments receive only `(data,
length, readonly)` from the host operation table and never reinterpret a VM
container header. A mutable `ref` parameter rejects readonly provenance before
entry; this explicit ABI revision changes no Xray value tag or native Slice
layout.
Each Xi function now carries its exact immutable SemanticPlan function index.
Hosted adapters use that record to promote a borrowed reference result before
releasing adapter-owned arguments. The index is compiler-internal and the
balanced retain changes no public value representation, calling convention,
parameter list, or native return ABI.
Task 260 carries recursive typed-Json schemas across VM bytecode and generated
C. Decoding a derived value struct writes its existing native aggregate layout;
heap materialization uses the registered type destructor and storage promoter.
This adds no public field, tag, calling-convention change, or per-instance
metadata to the target ABI.
Task 275 requires a current frozen coroutine plan before AOT may lower an
uncaptured closure to a direct C symbol. A missing, stale, incomplete, or
suspendable plan keeps the runtime closure representation; AOT must not
rediscover synchronous behavior from Xi. This tightens compiler authority
without changing the public closure layout or calling convention.
Representation refinement is likewise retained as one immutable authority per
module, bound to the exact SemanticPlan, TargetPlan, target policy, and backend
materialization. AOT prepare runs only after every required BOX or UNBOX is
present at its recorded source and use, and rejects a missing, extra, reordered,
or stale adapter before ABI planning. This adds no public value representation,
layout, or calling-convention change.
The C emission projection schema 11 preserves the exact materialization recipe
and immutable byte payload for every verified String literal row. CGen
mechanically consumes that row and cannot recover literal bytes, a dynamic
tag, field spelling, or ownership from mutable Xi values. Missing, extra,
reordered, stale, or incorrectly spelled rows fail before emission; this does
not authorize general owned Strings, tuples, or object bodies.
Schema 11 additionally projects the sealed `StringBuilder()` Target call as an
owned `TAGGED`/`XrValue` result with the fixed zero-argument `xrt_strbuf_new`
recipe. Ordinary and coroutine CGen mechanically consume that recipe; neither
may rediscover the builtin by Xi auxiliary text or fall back to generic
allocation. The recipe grants no generic builtin, object layout, root-map,
cleanup, alias, or allocation-table authority.
Schema 11 also preserves the exact borrowed `TAGGED`/`XrValue` row for a frozen
direct-local shared callee token. CGen consumes that immutable row mechanically;
it cannot infer callable representation from Xi type or representation state.
The live materialization verifier walks the caller parent chain to the frozen
callee's first lexical shared-slot owner and requires that owner's unique child
and slot pointer to match, so root-owned sibling helpers are accepted without
name or type guessing. The row grants no closure body, allocation, root, or
cleanup authority.
For an exact scalar `XI_CHAN_TRY_RECV`, schema 11 preserves the receiver semantic
value and exact scalar unbox helper spelling. Sync and coroutine CGen consume
that recipe mechanically; they cannot infer it from Xi type/representation or
fall back to a legacy adapter. This grants no Channel object layout, receive
scheduling, ownership transfer, aggregate/tuple payload, root, or cleanup
authority. For the exact String byte-slice intrinsic, schema 11 preserves the
borrowed `xr_span_t` view, its source semantic value, and the fixed
`xrt_span_from_string_bytes` recipe. CGen has no selector-, alias-, or
type-derived fallback; this grants no generic String method or Slice ABI.
An AOT cross-execution transfer row binds its site and payload to exactly one
representation authority. A TargetPlan value binding and a legacy value row
are mutually exclusive; the only accepted legacy rows are the independently
verified enum-ordinal and backend representation-adapter families. Transfer
verification reconstructs the site, payload, mode, action, storage domains,
proof identities, and evidence without calling the prepare collector. Missing,
duplicate, swapped, backend-only, or non-durable value authorities fail closed.
This tightens internal plan verification without changing the public target ABI.

Target semantics are selected before analysis, Xi lowering, generated-C
emission, and native linking:

The build and verification entry points derive one frozen TargetProfile from
the numeric toolchain target and canonical runtime/provider contracts, then
pass that same authority into AOT. A target spelling cannot reconstruct or
override ABI facts after planning, and an unavailable profile fails before
emission rather than falling back to compiler-host layout.

- T1: every supported target profile supplies one pointer width and byte order;
  target `usize`, pointer, aggregate, and native-load layouts derive from that
  profile rather than from the compiler host.
- T2: the native `Slice<T>` value ABI is always 16 bytes with 8-byte alignment,
  `data` at offset zero, and its signed 64-bit length at offset eight. ILP32 C
  targets carry explicit padding; they must not silently expose a 12-byte host
  structure against the frozen Xi representation.
- T3: portable SIMD reinterpretation is byte-order neutral. Logical byte zero
  is the least-significant byte of logical numeric lane zero on every target;
  big-endian C lowering reconstructs lanes instead of treating native `memcpy`
  bit patterns as the language semantics.
- T4: a named target may be published only when its profile, C-toolchain triple,
  pointer width, and endianness agree. Unsupported SIMD modes fail closed.
  Toolchain discovery is provider-neutral: target identity lives in the shared
  toolchain model, while probing resolves an installed provider for that model.
  Automatic selection tries the ordered host/provider set by capability, not by
  executable presence alone. A rejected host provider may fall back to Zig, but
  the fallback must retain the already selected target ABI (including native
  `x86_64-windows-msvc`) and pass compile, SDK, runtime-link, native-run, and LTO
  probes before it is reported ready.
- T4a: x86 runtime SIMD dispatch probes CPU and OS state together. AVX-512F is
  selectable only when CPUID leaf 7 reports AVX-512F and XCR0 enables XMM, YMM,
  opmask, ZMM high-256, and high-16 ZMM state. Baseline, AVX2, and AVX-512F
  functions remain in separately attributed feature islands. Explicit static
  SIMD selection is provider-neutral compile intent: each verified provider
  emits its own flag dialect, while `dispatch` keeps the translation-unit
  baseline free of global AVX2/AVX-512 enablement. SIMD mode and features are
  part of both object and link-output cache identities. Providers that predate
  Clang's `evex512` feature spelling retain the same AVX-512 island through the
  portable `avx512f` function target instead of dropping the whole attribute.
  Clang AVX2 islands carry a 256-bit minimum-vector-width attribute so explicit
  256-bit Xi operations are not silently legalized as paired 128-bit work.
  In a static SIMD build, an explicitly `@inline` cross-module vector wrapper
  retains hidden external linkage plus the native always-inline contract; a
  runtime-dispatch wrapper remains baseline and calls separately attributed
  feature leaves.
  A module initializer is always emitted as an ordinary hidden external
  definition because a separately generated entry translation unit invokes
  dependency initializers. It never uses external inline-only linkage; this
  preserves an exact linkable provider on MSVC and every other C provider.
- T4b: `loongarch64-linux-musl` defaults to scalar because LSX is not implied
  by the base target triple. Explicit `--simd lsx` or `--simd native` adds
  `-mlsx`, uses the portable 128-bit lane contract, and may publish native
  vector evidence only after an LSX-capable target binary executes.
- T4c: `aarch64-linux-musl --simd sve` preserves the exact lane count of the
  fixed-width vector family and gives `U8xNative`, `U32xNative`, and
  `U64xNative` a bounded runtime-selected active prefix. Hardware vector
  lengths of 128 bits select 16 bytes, lengths from 256 through 511 bits
  select 32 bytes, and lengths of at least 512 bits select 64 bytes. Inactive
  storage is not part of the language value; VM and non-SVE AOT execute a
  zero-initialized 16-byte fallback. Explicit predicated SVE intrinsics remain
  enabled, while implicit LLVM loop and SLP vectorization stay disabled until
  their fixed-trip-count lowering is valid for non-power-of-two vector lengths.
- T4d: `--c-dialect c90` is valid only for the frozen restricted profile: an
  LP64 Linux or Darwin target, scalar lowering, `--freestanding --shared
  --emit-c-only`, no program main, and no reachable runtime, standard-library,
  native-input, or aggregate public-ABI dependency. Unsupported profiles fail
  before generated C is handed to a host compiler. Dialect identity is part of
  both object and link cache keys.
- T4e: Windows native provider selection freezes the runtime archive ABI before
  probe or link. MSVC resolves `windows-msvc` COFF `.lib` artifacts; Zig keeps
  the already selected native ABI and may consume the same probe-validated COFF
  archive for a `windows-msvc` target, while an explicit `windows-gnu` target
  resolves its own GNU-spelled artifact set. Runtime manifests reject an ABI or
  object-format mismatch even when the digest is valid. A true cross-target
  probe may reuse installed host headers, but it never advertises or links the
  host runtime into the cross artifact. The COFF AOT runtime archive contains
  only VM-neutral translation units: because COFF resolves every undefined
  symbol in a selected object before section garbage collection, VM registration
  code may not share an archive member with an AOT-reachable core helper.
- T4f: provider capability identity contains four independent code-shape
  states: force-inline, preserve-call, value-opacity, and compiler-fence. A
  required state participates in selection and probe-cache identity. An
  installed host provider is tried first; when it cannot satisfy the requested
  baseline, an installed or managed Zig provider may be selected while retaining
  the already frozen target ABI. Missing capabilities fail closed rather than
  being reported as honored. The generated-C adapters are typed integer/pointer
  identities and compiler-only scheduling constructs; they cannot introduce a
  pointer-to-integer ABI round trip, hosted runtime dependency, hardware fence,
  or C++ linkage change.
- T4g: a Windows multi-module freestanding relocatable artifact is one COFF
  object compiled from the verified amalgamated translation unit. COFF has no
  ELF/Mach-O-style partial-link operation, so this path performs no link stage,
  rejects external objects, system libraries, linker flags, and linker scripts,
  and preserves an explicitly configured post-compile objcopy step. This is an
  artifact-kind lowering, not permission to substitute an archive or DLL. A
  hosted fragment selected for direct MSVC compilation must contain no GNU
  statement expression and must pass a real MSVC compile probe; failure remains
  fail-closed and does not downgrade the already selected target ABI.
- T5: a scalar place may alias its source field only when the semantic value,
  AOT representation, and generated-C type are identical. A value-preserving
  conversion such as ILP32 `usize` to 64-bit `int` must materialize distinct
  storage before its address is taken.
- T6: cross-target hosted time queries lower to target-owned header code, while
  native builds retain the runtime's shared OS clock. A cross-target binary
  must never consume the compiler host's AOT support archive merely to read
  wall, monotonic, or process CPU time.
- T7: shared-library format, suffix, link flags, and symbol visibility derive
  from the selected target rather than the compiler host. A default C export
  is externally visible; a hidden C export must not leak into the public image.
- T8: typed numeric conversion carries source and target scalar identities plus
  the selected target pointer width from analysis through Xi and VM bytecode.
  Integer conversion is modulo the target width followed by an explicit
  two's-complement interpretation; integer-to-float and binary64-to-binary32
  use round-to-nearest, ties-to-even; binary32 NaNs use the canonical Xray
  payload; float-to-integer truncates toward zero only after a range proof and
  otherwise raises `XR_ERR_OVERFLOW`. VM and AOT C consume the shared numeric
  conversion core and must not substitute host-language signed casts, the host
  floating-point environment, canonical type spellings, or compiler-host
  pointer width for this evidence.
- T9: a first-class `CFn` call may lower to generated-C `musttail` only when the
  call is the return block's final owned instruction, any following error check
  has no ARC cleanup, and the caller and callee native C signatures match
  exactly, including hidden closure parameter, arity, return type, and every
  parameter type. Otherwise CGen emits the call once and returns its stored SSA
  result without replaying side effects.
- T10: a noncoroutine native function accepts `CFn` parameters as raw native
  entry pointers. A statically proven top-level noncapturing function converts
  directly to that pointer; an already-native `CFn` forwards unchanged. The
  boundary must not allocate an Xray closure or recover a function address from
  a closure-tagged `XrValue`, and unsupported conversions fail closed. The AOT
  value plan assigns every non-nullable `CFn` the single `RAWPTR`
  representation even when the source Xi opcode historically produced tagged
  storage; CGen rejects any `CFn` coercion whose frozen plan says otherwise.
- T11: hosted AOT gives every physical coroutine one execution arena. Generic
  and embedded-header heap allocations retain their normal RC behavior while
  the arena owns the complete residual graph at coroutine teardown. A value
  published to TRANSFERABLE, CONST_SHARED, or SYNC_SHARED storage must detach
  its entire owned graph, including native-class reference fields, before the
  source arena can be destroyed. Native class type registration therefore
  carries a generated storage-promoter callback beside its destructor; missing
  graph evidence is a hard contract failure, never root-only promotion. The
  generated entry owns the root arena and shuts it down on every exit path.
- T12: `XrValue` and every object crossing VM/generated-fragment code use the
  single versioned public value/object ABI. `XrObjHeader` is the first field,
  carries the canonical object kind and ownership counters, and is validated by
  compile-time size/alignment/offset assertions on both sides. A fragment may
  borrow these objects but cannot invent a second layout, retain an unowned
  runtime root, or bypass the declared argument/return ownership convention.
  Hosted Fragment ABI v7 is the only accepted fragment interface. Contiguous
  byte input crosses through `byte_span_view`, never through a cast of the host
  Array or Slice representation; its pointer is borrowed for the call only,
  length must fit the signed native Slice ABI, and readonly provenance makes a
  mutable `ref` argument invalid rather than silently copying or mutating it.
- T13: when a native ADT aggregate crosses a tagged direct-call parameter,
  `READ` constructs a temporary box with independently retained payload lanes;
  a consuming parameter transfers the lanes without retaining. The bridge may
  change representation but cannot silently duplicate or discard ownership.

The release evidence includes generated-C filetests, the eleven-case
cross-target smoke matrix, executed PowerPC64 big- and little-endian
portable-SIMD fixtures, 180-vector xxHash VSX KATs for both byte orders, and a
LoongArch64 LSX binary that executes the 180-vector KAT plus exact 49-symbol C
ABI oracle under QEMU's `la464` CPU model, plus AArch64 SVE binaries that run
the same 180-vector KAT and exact 49-symbol C ABI oracle at 128-, 256-, 384-,
and 512-bit vector lengths. Retained SVE assembly proves that the runtime-native
xxHash stripe and scramble values remain in sizeless vector SSA rather than
fixed aggregate stack storage. Emulation proves semantic and ABI correctness,
not native Power, LoongArch, or SVE performance. A compile-only
cross artifact is not sufficient to claim platform support. AVX-512F evidence
may retain exact generated C and assembly plus execution of the same dispatch
binary on an AVX2-only host, but native AVX-512F execution and performance
remain separate release gates. Windows x86_64 evidence additionally includes
native execution of all five xxHash CLI names, a 24-case byte-exact upstream CLI
differential, the exact 49-export PE gate, an executing C ABI oracle, and complete
31-sample alternating Xray/API throughput matrices. On the verified host, MSVC
passes the minimal C/link probe and directly compiles the Task 256 scalar hosted
fragment plus the amalgamated freestanding COFF object. General whole-program
generated C still contains a required GNU statement expression, so MSVC is
rejected for that broader shape before any capability claim is made. With the
explicit workspace Zig 0.16.0 candidate, automatic selection falls back to Zig,
passes compile, SDK, runtime-link, native-run, LTO, and all four code-shape
capability probes, and preserves the `x86_64-windows-msvc` ABI. Without an
installed/configured Zig candidate the same general request reports unavailable;
the compiler core does not download a provider.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c a268bec7948a3a9cecf081d63d51545e3fe41af0b6ab32a9a38b21b031127da0
anchor-sha256: src/aot/xaot_prepare.c f025631467ed05ae8d5174765a5cbf43e13d5caa48f0d8e34e7cf8683027fbcd
anchor-sha256: src/aot/xaot_verify.c 484e2f52275a5ab27e2376bc78007a46972ca1cc47faac2425bdc830b0d2788e
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 4e63b69f6828aefd149170dc7815da693d3ef88d7cefb182c807acacbbaa3dec
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 11e99944b4cf4a3cddad2830bb60f364731489d6ec6b8be4e8df584312e852da
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 360ec13b19e0f029330d5efeb558af16ff8fbe1ac93d9168e4782f375c47aad4
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c b4201c02fc214411accff3be9a6f92b394c9116f86b79fc2b41a5e576a4a7d65
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c 9011332cab0c4cff0952d954c9dd04a6bd45160df9d9549b08828dc13d7af422
anchor-sha256: src/aot/xi_cgen.c 9c5bff596ee7aee40f26e1d908879fe356102d4f2446808e701defd541a03846
anchor-sha256: src/aot/xrt_coll.h bd9c91aea11ce6404d343155acff044415f2b98dc4c9b1a234d972843551ced3
anchor-sha256: src/aot/xrt_core_freestanding.h affc5073dbbc70e2c834b0a34ee027357b911d21d8c72154ae7ec16a46908244
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: include/xray_hosted_fragment_abi.h bcf50466f8320c265a49c6776f669912b83ac4ac3d04f397d4f6c527f1ead02c
anchor-sha256: src/app/cli/xcmd_build.c 29d14812d1b2c8bac57a3a9ca061b1a66719a88ac8dd4f621b9e549d8cd73c12
anchor-sha256: src/app/toolchain/xtc_model.c 91a6446ae4ffcda1178a979849c38c835b3092b4f8fdbffbf928c474a5ee1ac6
anchor-sha256: src/app/toolchain/xtc_probe.c 8d1cb7212b432a7cefe7e3e3d202509c75dd84190e084c3e7d2a88af62ca4eb1
anchor-sha256: src/ir/xi.h c215b18a09e101d32ce662289ed2ac2b6b8d778c36e5c40cb1a6724d5a06f661
anchor-sha256: stdlib/simd/simd.xr 0eb9b7955449743c09f7ba122cce51f8a772bb426413cde53c991b0ec664af24
anchor-sha256: src/aot/xaot_coro.h 51edaa56bb72326f5bacd0998b00d505e0c0533190f4ba0289c10ee954049995
anchor-sha256: src/aot/xi_cgen_class_helpers.inc.c 495a6b15c1a963c95bc26d98f4791adf0b6d16c1b33660900587a07bf738d7a1
anchor-sha256: src/aot/xrt_provider_abi.h 4deebceb145b02ba5c5836c8688b9c4788a130ab9fd0856d7ecc21dbcd5ce840
