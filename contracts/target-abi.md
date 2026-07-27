# Cross-target ABI contract

Status: re-frozen after x86 gained explicit AVX-512F static and runtime-dispatch
plans, PowerPC64 gained an explicit VSX target plan for both byte orders, and
LoongArch64 gained an explicit LSX target plan with an executed target gate,
and AArch64 gained an explicit scalable SVE plan with multi-VL execution.
Portable SIMD values crossing hosted module shared slots recover their fixed
aggregate layout from the tagged reference; scalar, native, and cross-endian
lowering retain one lane-order contract.

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
- T4d: Windows native provider selection freezes the runtime archive ABI before
  probe or link. MSVC resolves only `windows-msvc` COFF `.lib` artifacts, while
  Zig normalizes native Windows to `windows-gnu` and resolves only COFF `.a`
  artifacts. Runtime manifests reject cross-spelled archives even when their
  digest is valid. A true cross-target probe may reuse installed host headers,
  but it never advertises or links the host runtime into the cross artifact.
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
remain separate release gates. An exact PE
export-set gate proves the Windows artifact and ABI shape, but not Windows
execution support.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c 77db5eea55ef7ed4a31553ac05bf7efa88490b9e5b428c6d8c296744c05b797f
anchor-sha256: src/aot/xaot_prepare.c 68e7efd04383a8adff9aee330b8fea3755036354c29d83f96e56014d22cbf5dd
anchor-sha256: src/aot/xaot_verify.c 394cc8c6c53c982413af6d8524e49cdf573da31b0d75fd23c4b13dbdadc2a423
anchor-sha256: src/aot/xi_cgen_abi_helpers.inc.c 215e6cad4d4454e8ccb842347f32ead184ddaef8783938189484978029a93082
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c d11c0870ce6ff89b3aa8b4b6d3af25c62fb4cb1908c494ef821e4bdbb5832a14
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c d1157f98c28dae8e820329d84748ebfcf2e4e7adf0ac73466e673bf882bb48eb
anchor-sha256: src/aot/xi_cgen_program_entry.inc.c 1086f8f9c3ca50d13dd6b448acc7d81f89aeb0b3486b13230b76155fc58ab98c
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c 68479cd8cd8feb284ca4267d157571398d49d22d5d892cdecf5aa3614c936cae
anchor-sha256: src/aot/xi_cgen.c a80e8fd0572ae8ab46d22acebe17b7b5b99d88cedaf48e73b39fb276f6e21c24
anchor-sha256: src/aot/xrt_coll.h 8104b8d30e016cbca6c948bfc83c2b358258ec7ee2309d1e7bd60c967c61e6a0
anchor-sha256: src/aot/xrt_core_freestanding.h 96f25c3a7e609fc4024a70680ad34d5223ad4f9bc70f9aaec4b08943fec4333a
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c 3f8773ba6fbbec81cb0e3645647510c174fe2d4203a5ff1e94e781797d986c29
anchor-sha256: src/app/toolchain/xtc_model.c ec2ddd6e5bdfb3373691bfa5b4470bc4a86ebac2ebe98d61d27e3472a254231e
anchor-sha256: src/app/toolchain/xtc_probe.c 324d305889aa4953096c26d0817b5f0dcadc7c216ccfce5823bbbd5388c42162
anchor-sha256: src/ir/xi.h 633ceecd4e038acfc29a5ff826ecbfebd2295dc902e807039aab0bbb8da24260
anchor-sha256: stdlib/simd/simd.xr 56b8ce818e05b8a08475452205187b5cd673f2d992d9299c8ede6be7871eba8b
