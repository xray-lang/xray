# Cross-target ABI contract

Status: re-frozen after x86 gained explicit AVX-512F static and runtime-dispatch
plans, PowerPC64 gained an explicit VSX target plan for both byte orders, and
LoongArch64 gained an explicit LSX target plan with an executed target gate.
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
- T4a: x86 runtime SIMD dispatch probes CPU and OS state together. AVX-512F is
  selectable only when CPUID leaf 7 reports AVX-512F and XCR0 enables XMM, YMM,
  opmask, ZMM high-256, and high-16 ZMM state. Baseline, AVX2, and AVX-512F
  functions remain in separately attributed feature islands.
- T4b: `loongarch64-linux-musl` defaults to scalar because LSX is not implied
  by the base target triple. Explicit `--simd lsx` or `--simd native` adds
  `-mlsx`, uses the portable 128-bit lane contract, and may publish native
  vector evidence only after an LSX-capable target binary executes.
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
ABI oracle under QEMU's `la464` CPU model. Emulation proves semantic and ABI
correctness, not native Power or LoongArch performance. A compile-only
cross artifact is not sufficient to claim platform support. AVX-512F evidence
may retain exact generated C and assembly plus execution of the same dispatch
binary on an AVX2-only host, but native AVX-512F execution and performance
remain separate release gates. An exact PE
export-set gate proves the Windows artifact and ABI shape, but not Windows
execution support.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c a1f5a96100a247cf77442d2a08455a582f806aef50a807da2c9c7a835ed5680c
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c fb4d2822ce87e5c633d87edc68d11edb74f07b7595973b955f03d6a72692263b
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c bd28a6c9d49bb212431880faaebfcfc04159b9f79acd3c2a2e9392e667540eb4
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c 68479cd8cd8feb284ca4267d157571398d49d22d5d892cdecf5aa3614c936cae
anchor-sha256: src/aot/xi_cgen.c 66e8c88b154cc77deee1ac623a576218bbcabdf11110142618d84b4fe18f876c
anchor-sha256: src/aot/xrt_coll.h 9cb8e646bfabc64087e5358284f2691d85e785880c325181e81f95780549b6aa
anchor-sha256: src/aot/xrt_core_freestanding.h b23ab157b9b3ee8ed1fd58087baaae038c58a3857e9e12ae5a1dbd0007d6c805
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c 5436f2433c1acc23c5b6da78f4dcc154dd102d0e9bedac8bf378887a6e65dc90
anchor-sha256: src/app/cli/xcli_toolchain.c b0e406826e26acc3be56db57842c5b81fe970fd4f38c087acc8bc2fe27d04469
