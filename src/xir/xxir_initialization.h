/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_initialization.h - Compiler-owned detached initialization obligations
 */
#ifndef XXIR_INITIALIZATION_H
#define XXIR_INITIALIZATION_H
#include "xxir.h"

/* A source-owned snapshot; never serialized or admitted as executable XIR.
 * Parent checkpoints denote the state before the indexed instruction. */
typedef struct XrXirInitializationRegion {
    struct XrXirInitializationRegion *next, *parent;
    XrXirFunction function;
    uint32_t first_instruction, first_block, checkpoint;
} XrXirInitializationRegion;

/* Requires structurally checked executable input and compiler-owned regions.
 * The function index names the executable owner; diagnostics are output only. */
XR_FUNC XrXirStatus xr_xir_initialization_check(const XrXirModule *module,
    uint32_t function, const XrXirInitializationRegion *regions,
    XrXirBudget *remaining, XrXirDiagnostic *diagnostic);
#endif // XXIR_INITIALIZATION_H
