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

typedef enum XiEnumLiteralKind {
    XI_ENUM_LITERAL_NULL,
    XI_ENUM_LITERAL_INT,
    XI_ENUM_LITERAL_FLOAT,
    XI_ENUM_LITERAL_BOOL,
    XI_ENUM_LITERAL_STRING,
} XiEnumLiteralKind;

typedef struct XiEnumMemberData {
    const char *name;
    XiEnumLiteralKind value_kind;
    int64_t int_value;
    double float_value;
    bool bool_value;
    const char *string_value;
    int payload_count;
} XiEnumMemberData;

typedef struct XiEnumData {
    const char *name;
    int base_type;
    uint32_t member_count;
    bool is_adt;
    int max_payload;
    void *runtime_type;
    XiEnumMemberData *members;
} XiEnumData;

/* Per-module compilation unit: holds init function and explicit metadata.
 * All metadata is produced during lowering; no post-hoc IR scanning. */
typedef struct XiModule {
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
    XiConstLiteral *slot_const_literals; /* [nslots] shared slot -> scalar const literal */
    uint16_t nslots;                     /* = init->nshared */
    /* Closure metadata for all closures in this module */
    XiClosureMeta **closure_metas; /* [nclosure_metas] */
    uint16_t nclosure_metas;
} XiModule;

/* Allocate a new XiModule. Caller owns the returned pointer. */
XR_FUNC XiModule *xi_module_new(const char *path, const char *name, XiFunc *init);

/* Free a module and its metadata arrays (does NOT free init/functions). */
XR_FUNC void xi_module_free(XiModule *mod);

/* ========== Closure Pass ========== */

/* Build XiClosureMeta for all closures in a function tree.
 * Assigns env_offset and cell_index for each capture.
 * Advances stage to XI_STAGE_CLOSED. */
XR_FUNC void xi_pass_close(XiFunc *f);

#endif  // XI_MODULE_H
