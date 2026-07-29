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
fails the generated-C SDK capability probe and automatic selection falls back to
Zig 0.16.0 while preserving the `x86_64-windows-msvc` ABI.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c 77db5eea55ef7ed4a31553ac05bf7efa88490b9e5b428c6d8c296744c05b797f
anchor-sha256: src/aot/xaot_prepare.c 0a2119ede579c5a66139a24bdb77628679fc97548f70a9a4b3221d6e5fb6e66e
anchor-sha256: src/aot/xaot_verify.c 394cc8c6c53c982413af6d8524e49cdf573da31b0d75fd23c4b13dbdadc2a423
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 16153b0fb4075148ae0ab458dda75b7b8980984f4ee0aed8c6e1b5ac97239c30
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 7bd7f9f776b53e7f1fad6bc594744ec018420917d1d2b62225691cf8d7a27c07
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c b487883db6de4c01c33bad0b08d6d4503831872f380b1e8936166d87ea44b59e
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c bc860359654ec6597cfbebe6fcd3944436af9b085010d3ebc92acac68a7c1601
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c dc2ff44cd2ee1b61989cec03a28a51ddc9cb848507a42d8f111634eca38422a3
anchor-sha256: src/aot/xi_cgen.c 78719ea140a4a23531cdee74c517c5b65617b788e58e7dc82d298a3904886418
anchor-sha256: src/aot/xrt_coll.h 6d5cb458264d4594c3a301fae439f5cced30ff4cbbd407b36fa24d780be84c3d
anchor-sha256: src/aot/xrt_core_freestanding.h 1b65a7607516fdd820508d27b5644e610485f476f2d2a7690c7bb5d91494fba0
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c d441773288383d3a1caf926edec49387a7d0988d19e3005f627ce812047f7de2
anchor-sha256: src/app/toolchain/xtc_model.c ec2ddd6e5bdfb3373691bfa5b4470bc4a86ebac2ebe98d61d27e3472a254231e
anchor-sha256: src/app/toolchain/xtc_probe.c 1feb4ecaa53dbc48ec5242734b6fb87740525b459b70477606f635f4590624a8
anchor-sha256: src/ir/xi.h 63b1b248ce49a131e24fd2305789f1641673f5178dc00159757b0fe7565b775f
anchor-sha256: stdlib/simd/simd.xr c0bdc2461081513bdd1f362ec717da405cfa47ee9ab738d12be6f07cc69f8db3
