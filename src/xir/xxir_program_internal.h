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
/* Only a completed proof publishes this owner. Root facts and worker eligibility
 * have different meanings even when both deny a particular worker call. */
typedef struct XrXirProgramFunctionRef {
    uint32_t entry, captures;
    XrXirType type;
} XrXirProgramFunctionRef;
typedef struct XrXirProgramPermission {
    uint32_t reference_begin, reference_count;
    uint32_t root_parameter_begin, root_parameter_count, intrinsic_root;
    bool requires_root, unresolved;
    XrXirStatus worker;
} XrXirProgramPermission;
typedef struct XrXirProgramPermissions {
    uint32_t function_count, slot_count;
    XrXirProgramPermission *entries;
    XrXirProgramFunctionRef *references;
    uint32_t *parameter_offsets;
    uint32_t *root_parameters;
    uint8_t *cell_roles;
    uint32_t root_parameter_count;
} XrXirProgramPermissions;
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
    XrXirProgramPermissions *permissions;
};
/* Both inputs require prior shape verification; this grants no execution authority.
 * Backend-specific callback state storage is not a canonical frame layout. */
XR_FUNC XrXirStatus xr_xir_compile_program_match(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirFunctionLayout *layouts, const XrXirArtifact *lowered);
/* Decode, lowering and comparison consume the same owner. Successful
 * verification retains neither the packet nor temporary artifacts. */
XR_FUNC XrXirStatus xr_xir_compile_program_proof_verify(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirProgramProof *proof, XrXirProgramPermissions **permissions);
XR_FUNC bool xr_xir_compile_program_retain(XrXirProgram *program);
/* Inspect sealed facts and the advertised bound during the actual callback view. */
XR_FUNC XrXirCallStatus xr_xir_instance_call_entry_admit(XrXirCallView *view, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count);
#endif // XXIR_PROGRAM_INTERNAL_H
