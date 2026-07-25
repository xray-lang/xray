# Cross-target ABI contract

Status: re-frozen after hosted module-owned fixed arrays gained direct static
data and full-slice lowering. Hosted imports retain the shared-slot ABI and
freestanding imports retain their explicit weak-data contract; the 243-case
link matrix and cross-target filetests were rerun.

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

The release evidence includes generated-C filetests plus executed i386 ILP32
and PowerPC64 big-endian xxHash KATs. A compile-only cross artifact is not
sufficient to claim platform support. An exact PE export-set gate proves the
Windows artifact and ABI shape, but not Windows execution support.

## Digest anchors

anchor-sha256: src/aot/xaot_link.c 666708eb8f9af5c3c5598a6b926108a4abfd7ffffc298052bf86dd1bb99a6e47
anchor-sha256: src/aot/xi_cgen_class_native_helpers.inc.c 4ed015b3b02ea5cbf9c1cb9e970b2dbd4ee23515d56cf052df88eb1b176ae265
anchor-sha256: src/aot/xi_cgen_dispatch_helpers.inc.c 2255d2f7fe0f736748aa406da59c0209364e3611ba98e08be2787c546c614a8e
anchor-sha256: src/aot/xi_cgen_struct_helpers.inc.c 68479cd8cd8feb284ca4267d157571398d49d22d5d892cdecf5aa3614c936cae
anchor-sha256: src/aot/xi_cgen.c df1a19a34b3584cc4d64c9193cc8d7d2294229b1be76c11c7a447a9dce029fd0
anchor-sha256: src/aot/xrt_coll.h 9cb8e646bfabc64087e5358284f2691d85e785880c325181e81f95780549b6aa
anchor-sha256: src/aot/xrt_core_freestanding.h e3b5b2ca46c5749096101fc86e7960feda4b3c273d342020b19e075bb80002c0
anchor-sha256: src/aot/xrt_time.h 4d65fd48c6014eebffd2747b89c42652a1f1380a24cddbb07d0f1f79fa2c6aa7
anchor-sha256: src/app/cli/xcmd_build.c 65a5e17007a746aec20a10409ba2a236ca12abeb20bc2cca81896ba9fbb2f2b4
anchor-sha256: src/app/cli/xcli_toolchain.c f26a954b090d3c2f9bc9c7fc51f2d8963859aaa6b3f5645616d6121d1c0359db
