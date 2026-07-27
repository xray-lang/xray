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
  functions remain in separately attributed feature islands.
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
anchor-sha256: src/aot/xaot_prepare.c 68e7efd04383a8adff9aee330b8fea3755036354c29d83f96e56014d22cbf5dd
anchor-sha256: src/aot/xaot_verify.c 394cc8c6c53c982413af6d8524e49cdf573da31b0d75fd23c4b13dbdadc2a423
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 600aa9d449de2371e02802238a548ee739700df41e8b3c9cf4bd6d8b52b00a18
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 083b2451c3b9fe41f0d1c770a29f1e5004e27fe13eb83c016c3871d976f25b24
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 2923f376636dd4ab963b3ed3203136e3d577fe5c5e25171db74844434ff005c9
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c 5762e9d9d29ffdccc4b3fdc161cd45f10c1af1c771ed01807ce7c3701ee1f610
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c dc2ff44cd2ee1b61989cec03a28a51ddc9cb848507a42d8f111634eca38422a3
anchor-sha256: src/aot/xi_cgen.c 2831c25d3a4ce00a76e609d6e13194bd7517817712ea72d5235f52179103f353
anchor-sha256: src/aot/xrt_coll.h 6d5cb458264d4594c3a301fae439f5cced30ff4cbbd407b36fa24d780be84c3d
anchor-sha256: src/aot/xrt_core_freestanding.h adcb132c62bdbb9c9282c0181a0689ac1dd5cfe03f8c84d9e02692d955707011
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c 19122939aa291b24ed6091cc2f66fb97af3d472a3c4b07b827237a633511fb80
anchor-sha256: src/app/toolchain/xtc_model.c ec2ddd6e5bdfb3373691bfa5b4470bc4a86ebac2ebe98d61d27e3472a254231e
anchor-sha256: src/app/toolchain/xtc_probe.c 1feb4ecaa53dbc48ec5242734b6fb87740525b459b70477606f635f4590624a8
anchor-sha256: src/ir/xi.h 9d8f229600dc4a6aeb6092ae88c767ef67f79865f278884654d501f5598009d7
anchor-sha256: stdlib/simd/simd.xr 56b8ce818e05b8a08475452205187b5cd673f2d992d9299c8ede6be7871eba8b
