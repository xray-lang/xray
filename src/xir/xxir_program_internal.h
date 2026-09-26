/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program_internal.h - Sealed program storage shared with instance driving
 *
 * KEY CONCEPT:
 *   Instances retain immutable descriptors until execution and cleanup finish.
 */
#ifndef XXIR_PROGRAM_INTERNAL_H
#define XXIR_PROGRAM_INTERNAL_H
#include "xxir_program.h"
#include "xxir_callable.h"
#include <stdatomic.h>
struct XrXirProgram {
    _Atomic(uint32_t) references;
    XrXirCallEntry *entries;
    uint32_t entry_count;
    XrXirDeclarations *declarations;
    uint32_t *order;
    uint32_t *module_slots;
    XrXirCodeLease code;
    XrXirCallableTypes *callables;
};
XR_FUNC bool xr_xir_program_retain(XrXirProgram *program);
#endif // XXIR_PROGRAM_INTERNAL_H
