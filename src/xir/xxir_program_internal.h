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
#include "xxir_effect_contract_internal.h"
#include "xxir_types.h"
#include "xxir_type_arena.h"
#include <stdatomic.h>
/* Only a completed proof publishes this owner. Root facts and worker eligibility
 * have different meanings even when both deny a particular worker call. */
typedef struct XrXirProgramFunctionRef {
    uint32_t entry, captures, instruction, site;
    XrXirType type;
} XrXirProgramFunctionRef;
typedef struct XrXirProgramGeneratorSite {
    uint32_t entry, arguments, instruction;
    XrXirType type;
} XrXirProgramGeneratorSite;

typedef struct XrXirProgramPermission {
    uint32_t reference_begin, reference_count;
    uint32_t generator_begin, generator_count;
    uint32_t root_parameter_begin, root_parameter_count, intrinsic_root;
    bool requires_root, unresolved;
    XrXirStatus worker;
} XrXirProgramPermission;
typedef struct XrXirProgramPermissions {
    uint32_t function_count, slot_count;
    XrXirProgramPermission *entries;
    XrXirProgramFunctionRef *references;
    XrXirProgramGeneratorSite *generators;
    uint32_t *parameter_offsets;
    uint32_t *root_parameters;
    uint8_t *cell_roles;
    uint32_t root_parameter_count, reference_count, generator_count;
    struct XrXirEffects *effects;
    XrXirFunctionLayout *layouts;
    XrXirTarget target;
    uint8_t packet_identity[32];
} XrXirProgramPermissions;
/* This consumes the complete Effects certificate before the enclosing owner.
 * Its destruction is shared by failed sealing and final Program retirement. */
XR_FUNC void xir_program_permissions_free(XrXirProgramPermissions *permissions);
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
/* The current Lowered body and layouts must correspond to the independently
 * checked input. Wire identity detects drift; it grants no semantic permission. */
XR_FUNC XrXirStatus xr_xir_compile_program_body_match(const XrXirCompileContext *context,
    const XrXirProgramPermissions *permissions, const XrXirArtifact *lowered);
/* Compare the current descriptor against the same independently checked input.
 * No allocator or cleanup callback runs during this final comparison. */
XR_FUNC XrXirStatus xr_xir_compile_program_descriptor_match(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirProgramPermissions *permissions);
/* Decode, lowering and comparison consume the same owner. Successful
 * verification transfers only required layout metadata, not temporary artifacts. */
XR_FUNC XrXirStatus xr_xir_compile_program_proof_verify(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirProgramProof *proof, XrXirProgramPermissions **permissions);
XR_FUNC bool xr_xir_compile_program_retain(XrXirProgram *program);
/* Only the real synthetic initializer can report missing publication after
 * its owned module initializer returned Unit. This is not a host-fault waiver. */
XR_FUNC bool xr_xir_instance_initialization_publication_missing(
    XrXirCallView *view, const XrXirAction *action);
/* Inspect sealed facts and the advertised bound during the actual callback view. */
XR_FUNC XrXirCallStatus xr_xir_instance_call_entry_admit(XrXirCallView *view, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count);
/* Revalidate the same selection while every actual Class loan is reserved. */
XR_FUNC XrXirCallStatus xr_xir_instance_call_entry_recheck(XrXirCallView *view, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count,
    const XirEffectInvocationSelection *prepared);
/* A live caller may prepare a weaker callable argument after complete actual
 * admission. This check constructs no argument and grants no execution. */
XR_FUNC XrXirCallStatus xr_xir_instance_argument_weakening_admit(XrXirCallView *view,
    XrXirType type, const XrXirValue *input);
#endif // XXIR_PROGRAM_INTERNAL_H
