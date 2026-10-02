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
#include "xxir_types.h"
#include "xxir_type_arena.h"
#include <stdatomic.h>
struct XrXirProgram {
    _Atomic(uint32_t) references;
    XrXirCompileContext context;
    XrXirCallEntry *entries;
    uint32_t entry_count;
    XrXirDeclarations *declarations;
    uint32_t *order;
    uint32_t initialization_count;
    uint8_t *active_modules;
    uint32_t *module_slots;
    XrXirCodeLease code;
    XrXirTypeArena *arena;
    const XrXirTypes *types;
};
/* Both inputs require prior shape verification; this grants no execution authority.
 * Backend-specific callback state storage is not a canonical frame layout. */
XR_FUNC XrXirStatus xr_xir_compile_program_match(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirFunctionLayout *layouts, const XrXirArtifact *lowered);
/* Decode, lowering and comparison consume the same owner. Successful
 * verification retains neither the packet nor temporary artifacts. */
XR_FUNC XrXirStatus xr_xir_compile_program_proof_verify(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirProgramProof *proof);
XR_FUNC bool xr_xir_compile_program_retain(XrXirProgram *program);
#endif // XXIR_PROGRAM_INTERNAL_H
