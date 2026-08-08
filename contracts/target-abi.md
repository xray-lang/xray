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
Task 260 carries recursive typed-Json schemas across VM bytecode and generated
C. Decoding a derived value struct writes its existing native aggregate layout;
heap materialization uses the registered type destructor and storage promoter.
This adds no public field, tag, calling-convention change, or per-instance
metadata to the target ABI.
The public bytecode-bundle ABI preserves the compiler module graph's complete
topological order. Source-backed modules carry embedded bytecode; native,
standard-library, and dynamically linked package modules occupy explicit slots
with null bytecode. The runtime initializes every non-entry slot in that order
before evaluating the entry, so graph-resolved import and re-export operands
retain identical module indices after source removal. A compressed runtime
module table or a separate preload list is not a valid bundle representation.
Payload-free enums returned by direct stdlib helpers use a declared compact
`int64_t` ordinal shim ABI. This is an internal provider boundary, not the
public tagged value ABI: an I64 consumer keeps the ordinal, while a tagged or
erased consumer receives the existing `XR_TAG_ENUM` representation backed by
one immutable scalar-layout sidecar. Invalid ordinals abort at that boundary;
enum tags, layout identifiers, names, and calling conventions do not drift.

Target semantics are selected before analysis, Xi lowering, generated-C
emission, and native linking:

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
  LP64 Linux or Darwin target, scalar lowering, `--profile freestanding
  --artifact shared-library --c-only`, no program main, and no reachable runtime, standard-library,
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
  a closure-tagged `XrValue`, and unsupported conversions fail closed.
- T11: hosted AOT gives every physical coroutine one execution arena. Generic
  and embedded-header heap allocations retain their normal RC behavior while
  the arena owns the complete residual graph at coroutine teardown. A value
  published to TRANSFERABLE, CONST_SHARED, or SYNC_SHARED storage must detach
  its entire owned graph, including native-class reference fields, before the
  source arena can be destroyed. Native class type registration therefore
  carries a generated storage-promoter callback beside its destructor; missing
  graph evidence is a hard contract failure, never root-only promotion. The
  generated entry owns the root arena and shuts it down on every exit path.
  Publication changes storage/synchronization domain without discarding the
  compiler-accounted number of live owners: TRANSFER preserves the nonnegative
  RC count, while SHARED converts that count into the equivalent negative
  atomic encoding. `XI_TUPLE_NEW` consumes its lanes on both backends, and AOT
  CGen must materialize the tuple with its verified storage plan before it can
  cross a coroutine boundary; a later runtime copy or root-only repair is not
  an accepted substitute.
- T12: `XrValue` and every object crossing VM/generated-fragment code use the
  single versioned public value/object ABI. `XrObjHeader` is the first field,
  carries the canonical object kind and ownership counters, and is validated by
  compile-time size/alignment/offset assertions on both sides. A fragment may
  borrow these objects but cannot invent a second layout, retain an unowned
  runtime root, or bypass the declared argument/return ownership convention.
- T13: `XI_STORE_FIELD` consumes its stored value on every backend. Native-class
  CGen therefore releases the previous reference field and transfers the
  compiler-provided owner without adding an implicit retain. If the source must
  remain live, including exact self-assignment, XI ARC must materialize the
  balancing retain before the consuming store. Native reference fields cannot
  use scalar field caching because doing so would hide intermediate consuming
  overwrites from ARC.

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
anchor-sha256: src/aot/xaot_prepare.c aafc236f21231b6509fca91e544d948f7ea2d06b059fd0c8101f0286f4a66bbd
anchor-sha256: src/aot/xaot_verify.c 7fe97d173ac206666af4d63052ee0abb41b26eac8451da308ab5f8110fad6e16
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 5cafb87f5a28356200cdd737e84e7e7f6896b7c8ec97461262e872cb5f05b698
anchor-sha256: src/aot/xi_cgen_class_helpers.inc.c 495a6b15c1a963c95bc26d98f4791adf0b6d16c1b33660900587a07bf738d7a1
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 618628f3a903675d603562435acafe6ee76baeea66f1ecf4524169b55dd85b2f
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 2803a9189904ea6e4dc3100bb741aae456d32b544cdcb74a62ffcb0dff04af14
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c bd887f744b45816c5ba7db5e965c3df6ece1c0197770988b92120b3d468af907
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c 4ad72c173bf880b48ff9c232aee4d3648c7848d749c0a34881373e0e9ac051f7
anchor-sha256: src/aot/xi_cgen.c d5def56c508774fe4880711e0c729d257827a6afe398b8cd02bffe20f4b6612f
anchor-sha256: src/aot/xrt_coll.h d486fb006def4b702fc9a2e47642f954a4d9e5dacbbf4a7e00f938b83d0a2d30
anchor-sha256: src/aot/xrt_core_freestanding.h 13af59f530ac1a77bb9f050bb391a65cbaebe941d77efd4f65043c4d811c7e00
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c 3d2c8d70f9858e26ce1cbdc9d40451cb052ab2628417e6a556e00cfa2ed886c9
anchor-sha256: src/app/toolchain/xtc_model.c 91a6446ae4ffcda1178a979849c38c835b3092b4f8fdbffbf928c474a5ee1ac6
anchor-sha256: src/app/toolchain/xtc_probe.c 4e59251373523665674da4c863fac624fadef9cf077ebf8b6e9301768573490d
anchor-sha256: src/ir/xi.h 2476bba606a02c2e036f88c02c623ae19b62e17abcb471bbb563d531e8dc0523
anchor-sha256: stdlib/simd/simd.xr 0eb9b7955449743c09f7ba122cce51f8a772bb426413cde53c991b0ec664af24
