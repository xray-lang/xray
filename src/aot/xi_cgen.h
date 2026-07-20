/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen.h - Xi IR to C code generation
 *
 * Translates typed SSA IR (XiFunc) directly to C source code,
 * bypassing bytecode and the machine-code Xm builder entirely.
 * Hosted generated code includes xrt.h for the full value/runtime surface.
 * Freestanding generated code includes xrt_core_freestanding.h for the
 * no-libc scalar ABI surface.
 */

#ifndef XI_CGEN_H
#define XI_CGEN_H

#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Opaque codegen context — holds all mutable state for one C-generation
 * session.  Created once by the AOT driver, shared across module calls,
 * then freed.  No file-scope globals. */
typedef struct XiCgenCtx XiCgenCtx;
typedef struct XaotBundle XaotBundle;
typedef struct XaotTarget XaotTarget;

typedef struct XiCgenCoroFrameStats {
    uint32_t coroutine_count;
    size_t total_frame_bytes;
    size_t max_frame_bytes;
    uint32_t total_roots;
    uint32_t total_releases;
    uint32_t max_roots;
    uint32_t max_releases;
} XiCgenCoroFrameStats;

typedef struct XiCgenStats {
    uint32_t functions_total;
    uint32_t functions_native_abi;
    uint32_t functions_tagged_abi;
    uint32_t functions_coro_abi;
    uint32_t boxed_adapters;
    uint32_t sync_go_wrappers;
    uint32_t xi_box_ops;
    uint32_t xi_unbox_ops;
} XiCgenStats;

/* Per-function abstraction-cost residue categories (task 217 §3.3, R1–R6).
 * These are the shapes @zero_cost forbids in the generated AOT code; the
 * enum order is the stable dump/diagnostic order and doubles as the allow-mask
 * bit index (1u << category). */
typedef enum XiResidueCategory {
    XI_RESIDUE_R1_RUNTIME_CALL = 0, /* non-whitelisted xrt_* runtime helper call */
    XI_RESIDUE_R2_HEAP_ALLOC,       /* heap / runtime allocation */
    XI_RESIDUE_R3_PENDING_ERROR,    /* pending-error check branch */
    XI_RESIDUE_R4_BOUNDS_PANIC,     /* bounds-panic branch (missing range evidence) */
    XI_RESIDUE_R5_BOX_UNBOX,        /* XrValue box / unbox */
    XI_RESIDUE_R6_LANES_ROUNDTRIP,  /* aggregate<->native vector round-trip (_lanes) */
    XI_RESIDUE_CATEGORY_COUNT,
} XiResidueCategory;

/* One recorded residue occurrence: the source line that produced it plus a
 * static missing-evidence hint whose vocabulary matches task-213 evidence
 * diagnostics (range / escape / effect). */
typedef struct XiResidueEntry {
    uint8_t category; /* XiResidueCategory */
    uint32_t line;    /* source line (0 = unknown) */
    /* owned: static string literal (indefinite lifetime, never dangles) */
    const char *reason;
} XiResidueEntry;

/* Per-function residue record.  func_name / source_file are borrowed from the
 * XiFunc / XiModule arena; residue records are produced and consumed
 * (--dump-residue + @zero_cost verify) within a single build, before IR/module
 * teardown, so the borrow never outlives its backing store. */
typedef struct XiFuncResidue {
    /* owned: XiFunc.name / XiModule.path arena; build-scoped, consumed pre-teardown */
    const char *func_name;
    /* owned: XiModule.path arena; build-scoped, consumed pre-teardown */
    const char *source_file;
    uint32_t counts[XI_RESIDUE_CATEGORY_COUNT];
    XiResidueEntry *entries;
    uint32_t nentries;
    uint32_t entries_cap;
    bool has_zero_cost;            /* function carries @zero_cost */
    uint32_t zero_cost_allow_mask; /* bitmask of exempted categories (1u<<category) */
} XiFuncResidue;

/* Stable short ("R1") and human labels for a residue category. */
XR_FUNC const char *xi_residue_category_short(XiResidueCategory category);
XR_FUNC const char *xi_residue_category_label(XiResidueCategory category);

typedef enum XiCgenTypeNameProfile {
    XI_CGEN_TYPE_NAMES_NONE = 0,
    XI_CGEN_TYPE_NAMES_PUBLIC,
    XI_CGEN_TYPE_NAMES_ALL,
} XiCgenTypeNameProfile;

/* Lifecycle */
XR_FUNC XiCgenCtx *xi_cgen_ctx_new(void);
XR_FUNC void xi_cgen_ctx_free(XiCgenCtx *ctx);
XR_FUNC void xi_cgen_ctx_set_aot_bundle(XiCgenCtx *ctx, const XaotBundle *bundle);
XR_FUNC void xi_cgen_ctx_set_target(XiCgenCtx *ctx, const XaotTarget *target, bool simd_active);
XR_FUNC void xi_cgen_ctx_set_emit_main(XiCgenCtx *ctx, bool emit_main);
XR_FUNC void xi_cgen_ctx_set_freestanding_profile(XiCgenCtx *ctx, bool freestanding);
XR_FUNC void xi_cgen_ctx_set_type_name_profile(XiCgenCtx *ctx, XiCgenTypeNameProfile profile);
/* Enable per-function residue capture/scan (task 217 P2/P3).  Off by default so
 * ordinary builds pay no capture overhead. */
XR_FUNC void xi_cgen_ctx_set_residue_tracking(XiCgenCtx *ctx, bool enabled);
XR_FUNC bool xi_cgen_has_error(const XiCgenCtx *ctx);
XR_FUNC XiCgenCoroFrameStats xi_cgen_coro_frame_stats(const XiCgenCtx *ctx);
XR_FUNC XiCgenStats xi_cgen_stats(const XiCgenCtx *ctx);

/* Per-function residue records collected while emitting bodies (task 217 P2).
 * Returns the internal array (owned by ctx, valid until xi_cgen_ctx_free) and
 * writes its length to *count.  NULL/0 when no bodies were emitted. */
XR_FUNC const XiFuncResidue *xi_cgen_func_residues(const XiCgenCtx *ctx, size_t *count);

/* Render the collected per-function residue records as a TSV (task 217 P2
 * --dump-residue).  Caller frees the malloc'd string with xr_free.  Returns
 * NULL only on allocation failure. */
XR_FUNC char *xi_cgen_residue_dump(const XiCgenCtx *ctx);

/* Verify every @zero_cost function's residue against its allow mask (task 217
 * P3).  Emits an error[E0655] diagnostic per violating function to stderr
 * (listing each forbidden residue category + missing evidence) and returns the
 * number of violating functions (0 = all contracts hold).  Requires residue
 * tracking to have been enabled before emission. */
XR_FUNC int xi_cgen_verify_zero_cost(const XiCgenCtx *ctx);

/* Generate a complete standalone C file (single-module fast path):
 *   #include "xrt.h" + forward decls + bodies + main()
 * Suitable for: cc -o out file.c */
XR_FUNC void xi_cgen_program(XiCgenCtx *ctx, FILE *out, struct XiModule *module);

/* ========== Multi-module API ========== */

/* Emit the common C header (includes, defines). Call once per file. */
XR_FUNC void xi_cgen_header(XiCgenCtx *ctx, FILE *out);

/* Resolve cross-module imports.  Populates ctx internal import table
 * from the module graph.  Must be called before C generation. */
XR_FUNC void xi_cgen_resolve_module_imports(XiCgenCtx *ctx, struct XiModule **modules,
                                            int nmodules);

/* Emit main() calling module inits in topo order. */
XR_FUNC void xi_cgen_main(XiCgenCtx *ctx, FILE *out, struct XiModule **modules, int n,
                          int entry_index);

/* Emit one self-contained translation unit for modules[mod_index] (114
 * separate compilation): includes, external forward declarations for every
 * module's functions and shared-slot arrays, this module's definitions, and
 * (for the entry module) main().  Reuse a single ctx across every unit of a
 * bundle so function ids stay globally consistent. */
XR_FUNC void xi_cgen_module_tu(XiCgenCtx *ctx, FILE *out, struct XiModule **modules, int nmodules,
                               int mod_index, int entry_index);

/* Emit a public C header for every @c_export wrapper in a bundle.  The header
 * contains only C ABI declarations, not generated runtime internals. */
XR_FUNC void xi_cgen_c_export_header(XiCgenCtx *ctx, FILE *out, struct XiModule **modules,
                                     int nmodules, const char *guard);

/* Emit static const xrt_str_t definitions for every string literal
 * interned while emitting module bodies.  Multi-module flow: buffer
 * module/main output in a memstream, emit these defs after the header,
 * then append the buffered bodies (definitions must precede uses). */
XR_FUNC void xi_cgen_emit_str_literal_defs(XiCgenCtx *ctx, FILE *out);

#endif  // XI_CGEN_H
