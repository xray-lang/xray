/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_module.h - Xi IR module metadata and slot map types
 *
 * Extracted from xi.h to keep the main IR header within the 800-line limit.
 * XiModule holds per-module compilation metadata (exports, slot mappings,
 * closure metadata).  XiSlotMap bridges Xi IR values to bytecode registers.
 */

#ifndef XI_MODULE_H
#define XI_MODULE_H

#include "xi.h"
#include "../plan/semantic/xr_program_semantic_closure.h"

struct XrProgramSemanticClosure;
struct XrScalarCallDecision;
struct XrI64OverflowDecisionTable;
struct XrTargetProfile;

/* ========== Module Metadata ========== */

/* Import binding classification. */
typedef enum XiBindingKind {
    XI_BIND_VALUE,     /* ordinary value (variable, constant) */
    XI_BIND_FUNCTION,  /* function declaration */
    XI_BIND_CLASS,     /* class declaration */
    XI_BIND_NAMESPACE, /* whole-module import (import mod) */
} XiBindingKind;

/* Explicit export entry: one per module-level exported binding. */
typedef struct XiModuleExport {
    const char *name;          /* exported identifier (e.g. "square") */
    uint16_t shared_slot;      /* slot in module's shared array */
    int16_t cell_index;        /* cross-module cell table index (-1 = N/A) */
    XiFunc *function;          /* non-NULL if this export is a function */
    XiClassData *class_data;   /* non-NULL if this export is a class */
    struct XrType *value_type; /* inferred type of the exported value */
    bool is_live_binding;      /* true for mutable export (re-assignable) */
} XiModuleExport;

typedef struct XiEnumMemberData {
    const char *name;
    uint32_t ordinal;
    int payload_count;
    const char **payload_names;
    struct XrType **payload_types;
} XiEnumMemberData;

typedef struct XiEnumData {
    const char *name;
    uint32_t member_count;
    bool is_adt;
    int max_payload;
    uint32_t layout_id;
    const char **type_param_names;
    uint8_t type_param_count;
    void *runtime_type;
    XiEnumMemberData *members;
} XiEnumData;

/* Per-module compilation unit: holds init function and explicit metadata.
 * All metadata is produced during lowering; no post-hoc IR scanning. */
typedef struct XiModule {
    char *identity;     /* durable module identity (owned) */
    const char *path;   /* source file path */
    const char *name;   /* C-safe identifier (e.g. "math_lib") */
    XiFunc *init;       /* module init function (top-level) */
    XiFunc **functions; /* all top-level functions (init's children) */
    uint16_t nfuncs;
    XiClassData **classes; /* all class descriptors lowered in this module */
    uint16_t nclasses;
    XiModuleExport *exports; /* explicit export table */
    uint16_t nexports;
    /* Shared-slot mappings: populated during lowering, consumed by C codegen.
     * Indexed by shared slot number (0..nslots-1).  NULL entries mean the
     * slot holds a plain value with no specialized module metadata. */
    XiFunc **slot_funcs;                 /* [nslots] shared slot -> XiFunc* */
    XiClassData **slot_classes;          /* [nslots] shared slot -> XiClassData* */
    XiEnumData **slot_enums;             /* [nslots] shared slot -> XiEnumData* */
    XiImportRef **slot_imports;          /* [nslots] shared slot -> XiImportRef* */
    XiConstLiteral *slot_const_literals; /* [nslots] shared slot -> const literal/static data */
    XiConstLiteral *slot_shared_initializers; /* [nslots] static initial value for shared slots */
    uint16_t nslots;                          /* = init->nshared */
    const char **global_asm_templates;        /* module-level top-level asm templates */
    uint16_t nglobal_asm;
    /* Closure metadata for all closures in this module */
    XiClosureMeta **closure_metas; /* [nclosure_metas] */
    uint16_t nclosure_metas;
    /* Pointer-free source identity copied from the immutable TypedProgram.
     * This binds an
     * attached PSC to the module that was actually lowered. */
    XrProgramSemanticModuleInput source_semantic_module;
    bool source_semantic_module_present;
    /* Frozen target-neutral authority reference, pointer-free scalar
     * decision, and exact target used to verify that decision. XiModule owns
     * these references for the Xi lifetime; later compiler stages may retain
     * the same immutable authorities. */
    struct XrProgramSemanticClosure *program_semantic_closure;
    uint32_t psc_module_index;
    struct XrScalarCallDecision *scalar_call_decision;
    struct XrI64OverflowDecisionTable *i64_overflow_decisions;
    struct XrTargetProfile *scalar_target_profile;
} XiModule;

/* Allocate a new XiModule. Caller owns the returned pointer. */
XR_FUNC XiModule *xi_module_new(const char *path, const char *name, XiFunc *init);

/* Replace the durable identity. Untyped, malformed, or missing identities fail closed. */
XR_FUNC bool xi_module_set_identity(XiModule *mod, const char *identity);

/* Free a module and its metadata arrays (does NOT free init/functions). */
XR_FUNC void xi_module_free(XiModule *mod);

/* ========== Closure Pass ========== */

/* Build XiClosureMeta for all closures in a function tree.
 * Assigns env_offset and cell_index for each capture. The consuming stage
 * transition verifies and publishes XI_STAGE_CLOSED. */
XR_FUNC void xi_pass_close(XiFunc *f);

#endif  // XI_MODULE_H
