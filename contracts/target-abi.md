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

Target semantics are selected before analysis, Xi lowering, generated-C
emission, and native linking:

- T1: every supported target profile supplies one pointer width and byte order;
  target `usize`, pointer, aggregate, and native-load layouts derive from that
  profile rather than from the compiler host.
- T2: the native `Slice<T>` value ABI is always 16 bytes with 8-byte alignment,
  `data` at offset zero, and its signed 64-bit length at offset eight. ILP32 C
  targets carry explicit padding; they must not silently expose a 12-byte host
  structure against the frozen Xi representation. That pair carries no element
  type, so a consumer that must interpret elements (`string()`, concatenation,
  the shared formatter) receives a typed borrowed `xrt_array_t` view built at
  the use site instead. The view is a display shape, not a second Slice ABI:
  slices continue to cross call, iterator, and native-class boundaries as the
  16-byte pair.
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
  host runtime into the cross artifact.
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
passes the minimal C/link probe but does not compile the canonical generated-C
dialect (including the currently required GNU statement expression), so it is
rejected before capability claims are made. With the explicit workspace Zig
0.16.0 candidate, automatic selection falls back to Zig, passes compile, SDK,
runtime-link, native-run, LTO, and all four code-shape capability probes, and
preserves the `x86_64-windows-msvc` ABI. Without an installed/configured Zig
candidate the same request reports unavailable; the compiler core does not
download a provider.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c 77db5eea55ef7ed4a31553ac05bf7efa88490b9e5b428c6d8c296744c05b797f
anchor-sha256: src/aot/xaot_prepare.c 0a2119ede579c5a66139a24bdb77628679fc97548f70a9a4b3221d6e5fb6e66e
anchor-sha256: src/aot/xaot_verify.c 394cc8c6c53c982413af6d8524e49cdf573da31b0d75fd23c4b13dbdadc2a423
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 6bca7d61399352dc8de2ed3d0ad08e52f009c613ef86fa8dc38e154310c03ded
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 192f1144a0c2e0526ee864799172a74a6f066314a85b596a3261b9ae7eb1e9d8
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 888c31e4df025df2e31e90b79aa4f9f0b1191c74081ba107f78ea8bf9097658d
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c bc860359654ec6597cfbebe6fcd3944436af9b085010d3ebc92acac68a7c1601
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c dc2ff44cd2ee1b61989cec03a28a51ddc9cb848507a42d8f111634eca38422a3
anchor-sha256: src/aot/xi_cgen.c 78d76a0da985ac8b68d8f8890f38d926be33719a95cd1ba0eaa197b09e9baf35
anchor-sha256: src/aot/xrt_coll.h 0e5a8d95fbfa1b91e94a58d568cf70d09c8a6b2ac913f753e1f9b69b880947eb
anchor-sha256: src/aot/xrt_core_freestanding.h 0bc07a44d027e6a9a048f54104687f33ea0a18c45f4224482a9f6af7fb7a7ec1
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c 355d7252d4d0c677184c6d4fadf76a0171252493f6446e230f7d0efca115a8fb
anchor-sha256: src/app/toolchain/xtc_model.c 91a6446ae4ffcda1178a979849c38c835b3092b4f8fdbffbf928c474a5ee1ac6
anchor-sha256: src/app/toolchain/xtc_probe.c b62358f3ea7dce00cd2b5c5defeda92e0fed46dbe646eaf2095e52f2f13048d5
anchor-sha256: src/ir/xi.h 174c729c1c7dbe55a0bc2e6d866f8cd2cf8973d447e325ec97f14b3958218e7f
anchor-sha256: stdlib/simd/simd.xr 0eb9b7955449743c09f7ba122cce51f8a772bb426413cde53c991b0ec664af24
