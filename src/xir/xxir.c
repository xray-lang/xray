/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir.c - Owned and reverified XIR stage transitions
 *
 * KEY CONCEPT:
 *   Published artifacts do not retain construction buffers or frontend arenas.
 */

#include "xxir_internal.h"
#include "xxir_derivation_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_generic.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../base/xchecks.h"
#include <stdatomic.h>

const char *xr_xir_op_name(XrXirOp op) {
    switch (op) {
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) \
    case XR_XIR_##name: return #name;
#include "xxir_ops.def"
#undef XR_XIR_OP
    default: return NULL;
    }
}

const XrXirCompileContext *xr_xir_compile_artifact_context(const XrXirArtifact *artifact) {
    return artifact ? &artifact->context : NULL;
}

const XrXirModule *xr_xir_compile_artifact_module(const XrXirArtifact *artifact) {
    return artifact ? &artifact->module : NULL;
}

const XrXirTarget *xr_xir_compile_artifact_target(const XrXirArtifact *artifact) {
    return artifact && artifact->module.stage == XR_XIR_LOWERED ? &artifact->target : NULL;
}

const XrXirFunctionLayout *xr_xir_compile_artifact_layout(const XrXirArtifact *artifact, uint32_t function) {
    return artifact && artifact->layouts && function < artifact->module.function_count ?
        &artifact->layouts[function] : NULL;
}

void xr_xir_compile_artifact_free(XrXirArtifact *artifact) {
    if (!artifact)
        return;
    XrXirFunction *functions = (XrXirFunction *) artifact->module.functions;
    for (uint32_t i = 0; functions && i < artifact->module.function_count; ++i) {
        xr_compile_resources_free((void *) functions[i].name);
        xr_compile_resources_free((void *) functions[i].parameters);
        xr_compile_resources_free((void *) functions[i].blocks);
        xr_compile_resources_free((void *) functions[i].instructions);
        xr_compile_resources_free((void *) functions[i].operands);
        if (artifact->layouts) {
            xr_compile_resources_free((void *) artifact->layouts[i].offsets);
            xr_compile_resources_free((void *) artifact->layouts[i].parameters);
            xr_compile_resources_free((void *) artifact->layouts[i].owned_offsets);
        }
    }
    xir_source_facts_release(artifact->source_facts);
    xr_xir_compile_construction_free(artifact->construction);
    xr_compile_resources_free(artifact->layouts);
    xr_xir_compile_checked_packet_free(&artifact->checked_packet);
    xr_xir_compile_generics_free((XrXirGeneric *) artifact->module.generics, artifact->module.function_count);
    xr_xir_compile_declarations_free((XrXirDeclarations *) artifact->module.declarations);
    xr_xir_compile_types_free((XrXirTypes *) artifact->module.types);
    xr_xir_compile_provenance_free((XrXirProvenance *)artifact->module.provenance);
    if (artifact->module.defaults) {
        xr_compile_resources_free((void *)artifact->module.defaults->records);
        xr_compile_resources_free((void *)artifact->module.defaults);
    }
    xr_compile_resources_free(functions);
    xr_compile_resources_free(artifact);
}

static void *copy_bytes(const XrXirCompileContext *compile_context, const void *source, size_t size, XrXirStatus *allocation_status) {
    return xir_compile_copy(compile_context, source, size, allocation_status);
}

static bool clone_defaults(const XrXirCompileContext *compile_context, const XrXirDefaultTable *source, XrXirModule *destination, XrXirStatus *allocation_status) {
    if (!source) return true;
    XrXirDefaultTable *table = xir_compile_calloc(compile_context, 1, sizeof(*table), allocation_status);
    if (!table) return false;
    destination->defaults = table;
    table->records = copy_bytes(compile_context, source->records, (size_t)source->count * sizeof(*source->records), allocation_status);
    if (!table->records) return false;
    table->count = source->count;
    return true;
}

#include "xxir_construction.inc.c"

static XrXirProvenance *provenance_clone(const XrXirCompileContext *context,
    const XrXirProvenance *source, XrXirStatus *allocation_status);

static XrXirArtifact *source_artifact_clone(const XrXirCompileContext *context,
    const XrXirArtifact *source,XrXirStatus *status);

static XrXirArtifact *clone_module_receiving(const XrXirCompileContext *compile_context,
    const XrXirModule *source, const XrXirConstruction *construction,
    XrXirStatus *allocation_status, XrXirProvenance **owned_provenance) {
    XrXirArtifact *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), allocation_status);
    if (!copy)
        return NULL;
    XrXirFunction *functions = xir_compile_calloc(compile_context, source->function_count, sizeof(*functions), allocation_status);
    if (!functions) {
        xr_compile_resources_free(copy);
        return NULL;
    }
    copy->context = *compile_context;
    copy->evaluation_only=xir_effect_evidence_is_evaluation(source)?1u:0u;
    copy->module = (XrXirModule) {source->stage, functions, source->function_count, NULL, NULL, NULL, NULL, source->linkage_kind, NULL};
    XrXirGeneric *generics = NULL;
    if ((*allocation_status = xr_xir_compile_generics_clone(compile_context, source, &generics)) != XR_XIR_OK) {
        xr_xir_compile_artifact_free(copy); return NULL;
    }
    copy->module.generics = generics;
    XrXirTypes *types = NULL;
    if ((*allocation_status = xr_xir_compile_types_clone(compile_context, source->types, &types)) != XR_XIR_OK) {
        xr_xir_compile_artifact_free(copy); return NULL;
    }
    copy->module.types = types;
    *allocation_status = xir_construction_clone(compile_context, types, construction, &copy->construction);
    if (*allocation_status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return NULL; }
    XrXirDeclarations *declarations = NULL;
    if ((*allocation_status = xr_xir_compile_declarations_clone(compile_context, source->declarations, source->function_count, &declarations)) != XR_XIR_OK) {
        xr_xir_compile_artifact_free(copy);
        return NULL;
    }
    copy->module.declarations = declarations;
    if (!clone_defaults(compile_context, source->defaults, &copy->module, allocation_status)) {
        xr_xir_compile_artifact_free(copy); return NULL;
    }
    for (uint32_t i = 0; i < source->function_count; ++i) {
        const XrXirFunction *from = &source->functions[i];
        XrXirFunction *to = &functions[i];
        *to = *from;
        to->name = copy_bytes(compile_context, from->name, from->name_length, allocation_status);
        to->parameters = copy_bytes(compile_context, from->parameters,
                                   (size_t) from->parameter_count * sizeof(*from->parameters), allocation_status);
        to->blocks = copy_bytes(compile_context, from->blocks, (size_t) from->block_count * sizeof(*from->blocks), allocation_status);
        to->instructions = copy_bytes(compile_context, from->instructions,
                                     (size_t) from->instruction_count * sizeof(*from->instructions), allocation_status);
        to->operands = copy_bytes(compile_context, from->operands, (size_t) from->operand_count * sizeof(*from->operands), allocation_status);
        if (!to->name || (to->parameter_count && !to->parameters) ||
            !to->blocks || !to->instructions || (to->operand_count && !to->operands)) {
            xr_xir_compile_artifact_free(copy);
            return NULL;
        }
    }
    if (source->provenance) {
        if (owned_provenance) {
            if (!*owned_provenance || source->provenance != *owned_provenance)
                *allocation_status = XR_XIR_BAD_STRUCTURE;
            else if (!xir_compile_work(compile_context,
                sizeof(copy->module.provenance) + sizeof(*owned_provenance)))
                *allocation_status = XR_XIR_BUDGET;
            if (*allocation_status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return NULL; }
            copy->module.provenance = *owned_provenance; *owned_provenance = NULL;
        } else {
            copy->module.provenance = provenance_clone(compile_context, source->provenance, allocation_status);
            if (!copy->module.provenance) { xr_xir_compile_artifact_free(copy); return NULL; }
        }
    }
    return copy;
}

static XrXirArtifact *clone_module(const XrXirCompileContext *compile_context,
    const XrXirModule *source, const XrXirConstruction *construction, XrXirStatus *allocation_status) {
    return clone_module_receiving(compile_context, source, construction, allocation_status, NULL);
}

#include "xxir_provenance.inc.c"
#include "xxir_source_facts.inc.c"
#include "xxir_provenance_match.inc.c"
#include "xxir_types_lower.inc.c"
#include "xxir_checked_codec.inc.c"

static XrXirStatus transition_error(XrXirStatus status, XrXirDiagnostic *diagnostic) {
    if (diagnostic)
        *diagnostic = (XrXirDiagnostic) {status, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
    return status;
}

XrXirStatus xr_xir_compile_recheck_v2(const XrXirCompileContext *compile_context, const XrXirModule *checked, const XrXirConstruction *construction, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output || *output) return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);

    if (!checked || checked->stage != XR_XIR_CHECKED) return transition_error(XR_XIR_BAD_STAGE, diagnostic);
    XrXirCompileContext limits = *budget;
    XrXirStatus status = xr_xir_compile_verify_v2(&limits, checked, construction, diagnostic);
    if (status != XR_XIR_OK) return status;
    XrXirArtifact *copy = clone_module(compile_context, checked, construction, &allocation_status);
    if (!copy) return transition_error(allocation_status, diagnostic);
    copy->context = limits;
    status = source_facts_prepare(copy,diagnostic);
    if (status==XR_XIR_OK) status = xr_xir_compile_artifact_verify(copy, diagnostic);
    if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return status; }
    *output = copy; return XR_XIR_OK;
}

static XrXirStatus artifact_verify_owned_effects(const XrXirArtifact *artifact,
    XrXirEffects **output, const XrXirEffects *source_effects, XrXirDiagnostic *diagnostic);

/* Reject overlapping request and owner/output slots before any dereference or
 * acceptance. Address comparison only refuses aliases; it proves no input. */
static bool provenance_receiving_slot_alias(const XirProvenanceReceiving *receiving,
    const void *slot) {
    uintptr_t request=(uintptr_t)receiving,location=(uintptr_t)slot;
    return request<=location ? location-request<sizeof(*receiving) :
        request-location<sizeof(void *);
}

/* Only this producer's uniquely owned provenance is moved. Both complete
 * checks still reconstruct their own facts from the actual receiving body. */
XrXirStatus xr_xir_compile_recheck_provenance_take(const XrXirCompileContext *compile_context,
    const XrXirModule *checked, const XirProvenanceReceiving *receiving,
    XrXirProvenance **owned_provenance, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(compile_context) || !receiving || !output || !owned_provenance ||
        (void *)output == (void *)owned_provenance ||
        provenance_receiving_slot_alias(receiving,output) ||
        provenance_receiving_slot_alias(receiving,owned_provenance) || *output || !*owned_provenance)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (!checked || checked->stage != XR_XIR_CHECKED)
        return transition_error(XR_XIR_BAD_STAGE, diagnostic);
    const XrXirProvenance *candidate = *owned_provenance;
    if (checked->provenance != candidate || !xir_effect_kind_is_closed(candidate->kind) ||
        !candidate->source || candidate->source->context.resources != compile_context->resources)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    XrXirProvenance *receiving_owner = *owned_provenance; *owned_provenance = NULL;
    XrXirStatus status = XR_XIR_OK;
    XrXirArtifact *receiving_copy = NULL;
    if (!xir_compile_work(compile_context, sizeof(XrXirModule) + 3 * sizeof(receiving_owner) +
        sizeof(*receiving))) {
        status = transition_error(XR_XIR_BUDGET, diagnostic); goto done;
    }
    const XirProvenanceReceiving request=*receiving;
    XrXirModule receiving_module = *checked;
    receiving_module.provenance = receiving_owner;
    XrXirCompileContext limits = *compile_context;
    status = xr_xir_compile_verify_provenance_effects_v2(&limits, &receiving_module,
        request.construction, request.source_effects, diagnostic);
    if (status != XR_XIR_OK) goto done;
    receiving_copy = clone_module_receiving(compile_context, &receiving_module, request.construction,
        &status, &receiving_owner);
    if (!receiving_copy) { status = transition_error(status, diagnostic); goto done; }
    receiving_copy->context = limits;
    status = artifact_verify_owned_effects(receiving_copy, NULL, request.source_effects, diagnostic);
    if (status == XR_XIR_OK) { *output = receiving_copy; receiving_copy = NULL; }
done:
    xr_xir_compile_artifact_free(receiving_copy);
    xr_xir_compile_provenance_free(receiving_owner);
    return status;
}

static XrXirStatus transition_shape(const XrXirModule *input, XrXirStage source,
    const XrXirCompileContext *budget, const XrXirTarget *target, XrXirDiagnostic *diagnostic) {
    if (!input || input->stage != source)
        return transition_error(XR_XIR_BAD_STAGE, diagnostic);
    if (source == XR_XIR_CHECKED) {
        if (input->linkage_kind != XR_XIR_PROGRAM)
            return transition_error(XR_XIR_BAD_STAGE, diagnostic);
        if (input->generics || (input->types && input->types->interfaces))
            return transition_error(XR_XIR_BAD_STAGE, diagnostic);
        if (input->types) {
            if (input->types->count && !input->types->nodes) return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
            if (!xir_compile_work(budget,input->types->count)) return transition_error(XR_XIR_BUDGET,diagnostic);
        }
        XrXirLayout layout;
        XrXirStatus layout_status = xr_xir_compile_layout(budget, NULL, XR_XIR_I64, target, XR_XIR_LAYOUT_FRAME, &layout);
        if (layout_status != XR_XIR_OK) return transition_error(layout_status, diagnostic);
    }
    return XR_XIR_OK;
}

typedef XrXirStatus (*TransitionPacketWriter)(const XrXirArtifact *,
    XrXirCheckedPacket *, XrXirDiagnostic *);
typedef struct TransitionInputProof {
    TransitionPacketWriter writer;
    XrXirArtifact **owned_input;
    XrXirEffects **effects;
} TransitionInputProof;

/* The private reader transfers its fully verified, unaliased decoded owner.
 * Borrowed inputs still need an independent deep copy before any mutation. */
static XrXirStatus transition_owned(const XrXirModule *input, const XrXirConstruction *construction,
    const XrXirCompileContext *budget, XrXirArtifact **output,
    XrXirDiagnostic *diagnostic, const XrXirTarget *target, const TransitionInputProof *proof) {
    XrXirStage source = input->stage;
    XrXirStatus allocation_status = XR_XIR_OK, status = XR_XIR_OK;
    XrXirCompileContext limits = *budget;
    XrXirArtifact *copy;
    if (proof->owned_input) {
        copy = *proof->owned_input;
        if (!copy || &copy->module != input)
            return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
        *proof->owned_input = NULL;
    } else {
        copy = clone_module(budget, input, construction, &allocation_status);
        if (!copy) return transition_error(allocation_status, diagnostic);
    }
    copy->context = limits;
    budget = &limits;
    if (source == XR_XIR_CHECKED) {
        status = proof->writer(copy, &copy->checked_packet, diagnostic);
        if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return status; }
        if (!xir_compile_work(budget, copy->checked_packet.length)) {
            xr_xir_compile_artifact_free(copy); return transition_error(XR_XIR_BUDGET, diagnostic);
        }
        xr_sha256(copy->checked_packet.bytes, copy->checked_packet.length, copy->checked_identity);
    }
    if (copy->source_facts) { xir_source_facts_release(copy->source_facts);copy->source_facts=NULL; }
    copy->module.stage = source == XR_XIR_BUILT ? XR_XIR_CHECKED : XR_XIR_LOWERED;
    if (copy->module.stage == XR_XIR_LOWERED) {
        XrXirCompileContext projection_budget = limits;
        status = lower_nominal_types(&copy->module, &projection_budget);
        if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return transition_error(status, diagnostic); }
        XrXirConstruction *physical=NULL;
        status = xir_construction_lower(budget,copy->module.types,copy->construction,&physical);
        if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return transition_error(status, diagnostic); }
        xr_xir_compile_construction_free(copy->construction);copy->construction=physical;
        copy->target = *target;
        for (uint32_t f = 0; f < copy->module.function_count; ++f) {
            const XrXirFunction *function = &copy->module.functions[f];
            XrXirInstruction *instructions = (XrXirInstruction *) function->instructions;
            if (!xir_compile_work(budget, function->instruction_count)) {
                xr_xir_compile_artifact_free(copy); return transition_error(XR_XIR_BUDGET, diagnostic);
            }
            for (uint32_t i = 0; i < function->instruction_count; ++i) {
                if (instructions[i].op == XR_XIR_COPY)
                    instructions[i].op = xr_xir_type_is_owned(copy->module.types, instructions[i].type) ?
                        XR_XIR_OWNED_RETAIN : XR_XIR_SCALAR_COPY;
                else if (instructions[i].op >= XR_XIR_LOCAL_NEW && instructions[i].op <= XR_XIR_LOCAL_WRITE) {
                    XrXirType type = instructions[i].op == XR_XIR_LOCAL_WRITE ?
                        instructions[instructions[i].args[0] - function->parameter_count].type : instructions[i].type;
                    instructions[i].op = (XrXirOp) ((xr_xir_type_is_owned(copy->module.types, type) ? XR_XIR_OWNED_LOCAL_NEW :
                        XR_XIR_SCALAR_LOCAL_NEW) + instructions[i].op - XR_XIR_LOCAL_NEW);
                }
            }
        }
        status = xr_xir_compile_layout_build(copy);
        if (status != XR_XIR_OK) {
            xr_xir_compile_artifact_free(copy);
            return transition_error(status, diagnostic);
        }
    }
    if (status==XR_XIR_OK && copy->module.stage==XR_XIR_CHECKED)
        status=source_facts_prepare(copy,diagnostic);
    if (proof->effects && !xir_compile_work(budget, sizeof(proof->effects)))
        status = transition_error(XR_XIR_BUDGET, diagnostic);
    if (status == XR_XIR_OK)
        status = proof->effects ? artifact_verify_owned_effects(copy, proof->effects, NULL, diagnostic) :
            xr_xir_compile_artifact_verify(copy, diagnostic);
    if (status != XR_XIR_OK) {
        xr_xir_compile_artifact_free(copy);
        return status;
    }
    *output = copy;
    return XR_XIR_OK;
}

static XrXirStatus transition(const XrXirModule *input, const XrXirConstruction *construction, XrXirStage source,
    const XrXirCompileContext *budget, XrXirArtifact **output,
    XrXirDiagnostic *diagnostic, const XrXirTarget *target) {
    if (!xir_compile_context_valid(budget) || !output || *output)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    XrXirStatus status = transition_shape(input, source, budget, target, diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_verify_v2(budget, input, construction, diagnostic);
    if (status != XR_XIR_OK) return status;
    /* Complete admission precedes the private exact clone. Its encoder needs
     * no second admission; complete receiving checks precede publication. */
    const TransitionInputProof proof = {checked_encode, NULL, NULL};
    return transition_owned(input, construction, budget, output, diagnostic, target, &proof);
}

/* The reader owns and verifies the only input; no decoded view escapes. */
XR_FUNC XrXirStatus xr_xir_compile_checked_read_lower(const XrXirCompileContext *context,
    const void *bytes, size_t length, const XrXirTarget *target,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !output || *output)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_compile_checked_read(context, bytes, length, &checked, diagnostic);
    if (status == XR_XIR_OK)
        status = transition_shape(&checked->module, XR_XIR_CHECKED, &checked->context, target, diagnostic);
    const TransitionInputProof proof = {checked_encode,&checked,NULL};
    if (status == XR_XIR_OK)
        status = transition_owned(&checked->module, checked->construction, &checked->context, output, diagnostic, target, &proof);
    xr_xir_compile_artifact_free(checked);
    return status;
}

/* Both outputs remain private until packet, type, source, effects and every
 * physical layout check finish. Only the decoded owner can reach this path. */
XR_FUNC XrXirStatus xr_xir_compile_checked_read_lower_effects(const XrXirCompileContext *context,
    const void *bytes, size_t length, const XrXirTarget *target,
    XirLoweredEffects *output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !output || output->artifact || output->effects)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (!xir_compile_work(context, sizeof(XirLoweredEffects)))
        return transition_error(XR_XIR_BUDGET, diagnostic);
    XirLoweredEffects result = {0};
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_compile_checked_read(context, bytes, length, &checked, diagnostic);
    if (status == XR_XIR_OK)
        status = transition_shape(&checked->module, XR_XIR_CHECKED, &checked->context, target, diagnostic);
    const TransitionInputProof proof = {checked_encode, &checked, &result.effects};
    if (status == XR_XIR_OK)
        status = transition_owned(&checked->module, checked->construction, &checked->context,
            &result.artifact, diagnostic, target, &proof);
    xr_xir_compile_artifact_free(checked);
    if (status == XR_XIR_OK && !xir_compile_work(context, sizeof(result)))
        status = transition_error(XR_XIR_BUDGET, diagnostic);
    if (status != XR_XIR_OK) {
        xr_xir_compile_effects_free(result.effects);
        xr_xir_compile_artifact_free(result.artifact);
        return status;
    }
    *output = result;
    return XR_XIR_OK;
}

XrXirStatus xr_xir_compile_check_v2(const XrXirCompileContext *compile_context, const XrXirModule *built, const XrXirConstruction *construction, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    return transition(built, construction, XR_XIR_BUILT, budget, output, diagnostic, NULL);
}

/* A private prepared graph is consumed immediately by the complete checker.
 * Construction memory remains private until an independent owned copy passes. */
XR_FUNC XrXirStatus xr_xir_compile_check_refined_v2(const XrXirCompileContext *context,
    const XrXirModule *built, const XrXirConstruction *construction, const XrXirRootRefiner *refiner,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !output || *output || !refiner ||
        !refiner->update || !refiner->context) return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    XrXirStatus status = transition_shape(built,XR_XIR_BUILT,context,NULL,diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_structure_verify_v2(context,built,construction,diagnostic);
    XrXirEffects *effects = NULL;
    if (status == XR_XIR_OK) status = xr_xir_compile_effects_refine_verified(context,built,refiner,&effects);
    if (status == XR_XIR_OK) status = xr_xir_compile_verify_effects_v2(context,built,construction,effects,diagnostic);
    XrXirArtifact *copy = NULL;
    if (status == XR_XIR_OK) {
        copy = clone_module(context,built,construction,&status);
        if (copy) {
            copy->module.stage = XR_XIR_CHECKED;
            status = xr_xir_compile_verify_effects_v2(context,&copy->module,copy->construction,effects,diagnostic);
        }
    }
    xr_xir_compile_effects_free(effects);
    if (status==XR_XIR_OK) status=source_facts_prepare(copy,diagnostic);
    if (status != XR_XIR_OK) {
        xr_xir_compile_artifact_free(copy);
        if (diagnostic) diagnostic->status = status;
        return status;
    }
    *output = copy; return XR_XIR_OK;
}

#include "xxir_generated.inc.c"

XrXirStatus xr_xir_compile_lower(const XrXirArtifact *checked, const XrXirTarget *target,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!checked || !output || *output || !xir_compile_context_valid(&checked->context))
        return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    XrXirStatus status=transition_shape(&checked->module,XR_XIR_CHECKED,&checked->context,target,diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,diagnostic);
    const TransitionInputProof proof={checked->evaluation_only?checked_evaluation_encode:checked_encode,NULL,NULL};
    return status==XR_XIR_OK?transition_owned(&checked->module,checked->construction,
        &checked->context,output,diagnostic,target,&proof):status;
}

/* Accepting this private request consumes its only owner, including failure.
 * Complete input and receiving verification remain independent gates. */
XR_FUNC XrXirStatus xr_xir_compile_lower_take(XrXirArtifact **owned_input,
    const XrXirTarget *target, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!owned_input || !*owned_input || !output || output == owned_input || *output ||
        !xir_compile_context_valid(&(*owned_input)->context))
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    XrXirArtifact *checked = *owned_input;
    *owned_input = NULL;
    XrXirCompileContext limits = checked->context;
    XrXirStatus status = transition_shape(&checked->module, XR_XIR_CHECKED, &limits, target, diagnostic);
    if (status == XR_XIR_OK)
        status = xr_xir_compile_artifact_verify(checked, diagnostic);
    if (status == XR_XIR_OK && (checked->checked_packet.bytes || checked->checked_packet.length))
        status = transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    if (status == XR_XIR_OK) {
        status = xr_xir_compile_layout_verify(checked);
        if (status != XR_XIR_OK) status = transition_error(status, diagnostic);
    }
    const TransitionInputProof proof = {checked->evaluation_only?checked_evaluation_encode:checked_encode,&checked,NULL};
    if (status == XR_XIR_OK)
        status = transition_owned(&checked->module, checked->construction, &limits, output, diagnostic, target, &proof);
    xr_xir_compile_artifact_free(checked);
    return status;
}

static XrXirStatus artifact_verify_owned_effects(const XrXirArtifact *artifact,
    XrXirEffects **output, const XrXirEffects *source_effects, XrXirDiagnostic *diagnostic) {
    if (!artifact || (output && (*output || source_effects))) return XR_XIR_BAD_STRUCTURE;
    if (artifact->evaluation_only>1 ||
        !!artifact->evaluation_only!=xir_effect_evidence_is_evaluation(&artifact->module))
        return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    XrXirCompileContext compile_state = artifact->context;
    XrXirCompileContext *budget = &compile_state;
    XrXirCompileContext limits = *budget;
    if (artifact && artifact->module.stage == XR_XIR_LOWERED) {
        const XrXirCheckedPacket *packet = &artifact->checked_packet;
        if (!packet->bytes || packet->length < 64)
            return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
        if ((packet->length > SIZE_MAX) ||!xir_compile_work(&limits, packet->length))
            return transition_error(XR_XIR_BUDGET, diagnostic);


        uint8_t digest[32];
        xr_sha256(packet->bytes, packet->length, digest);
        if (memcmp(digest, artifact->checked_identity, sizeof(digest)))
            return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
        if (artifact->evaluation_only) {
            if (packet->length<76 || memcmp(packet->bytes,"XRCEV\0\0\0",8))
                return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
            const XirEvaluationRoot *root=&artifact->module.provenance->evaluation;
            uint32_t wanted[3]={root->caller,root->instruction,root->callee};
            if (!xir_compile_work(&limits,12)) return transition_error(XR_XIR_BUDGET,diagnostic);
            for (uint32_t i=0;i<3;++i) {
                const uint8_t *p=packet->bytes+64+4*i;
                uint32_t actual=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
                if (actual!=wanted[i]) return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
            }
        } else if (!memcmp(packet->bytes,"XRCEV\0\0\0",8))
            return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    }
    const XrXirProvenance *provenance=artifact->module.provenance;
    if (artifact->module.stage==XR_XIR_CHECKED && provenance && provenance->kind==XR_XIR_EVIDENCE_TEMPLATE) {
        XrXirStatus status=source_facts_match(&limits,artifact,diagnostic);
        if (status!=XR_XIR_OK) return transition_error(status,diagnostic);
        if (!output) return XR_XIR_OK;
    }
    if (artifact->source_facts && (!output || !provenance || provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE))
        return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    if (provenance && xir_effect_kind_is_closed(provenance->kind) && provenance->source &&
        provenance->source->module.provenance &&
        provenance->source->module.provenance->kind==XR_XIR_EVIDENCE_TEMPLATE) {
        const XrXirArtifact *source=provenance->source;
        const XirSourceFacts *facts=source->source_facts;
        /* Common provenance verification below checks this same Source body,
         * effects and permissions on every request. Bind its retained owner,
         * terminal and layout here without running those full checks twice.
         * A separately supplied derivation cannot stand in for stored facts. */
        XrXirStatus status=source_effects && facts && source_effects!=facts->effects?
            source_facts_match(&limits,source,diagnostic):source_facts_input_match(&limits,source,facts);
        if (status==XR_XIR_OK) status=xir_effects_source_terminal(&limits,&source->module,facts->effects);
        if (status==XR_XIR_OK) status=xr_xir_compile_layout_match_verified(source);
        if (status!=XR_XIR_OK) return transition_error(status,diagnostic);
        if (!source_effects) source_effects=facts->effects;
    }
    XrXirEffects *owned=NULL;
    XrXirStatus status = output ? xr_xir_compile_verify_receiving_effects_v2(&limits,
        xr_xir_compile_artifact_module(artifact),artifact->construction,source_effects,&owned,diagnostic) :
        xr_xir_compile_verify_provenance_effects_v2(&limits,xr_xir_compile_artifact_module(artifact),
            artifact->construction,source_effects,diagnostic);
    if (status != XR_XIR_OK) return status;
    status = xr_xir_compile_layout_match_verified(artifact);
    if (status!=XR_XIR_OK) { xr_xir_compile_effects_free(owned);return transition_error(status,diagnostic); }
    if (output) *output=owned;
    return XR_XIR_OK;
}
XrXirStatus xr_xir_compile_artifact_verify(const XrXirArtifact *artifact, XrXirDiagnostic *diagnostic) {
    return artifact_verify_owned_effects(artifact,NULL,NULL,diagnostic);
}

/* An explicit owning collector still returns a new complete derivation.
 * Persistent Source consumers use the separate lifetime lease; this owning
 * interface never returns the Artifact's retained immutable owner. */
XR_FUNC XrXirStatus xr_xir_compile_artifact_verify_owned_effects(const XrXirArtifact *artifact,
    XrXirEffects **output, XrXirDiagnostic *diagnostic) {
    if (!artifact || !output || *output) return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    if (!module || module->stage!=XR_XIR_CHECKED) return transition_error(XR_XIR_BAD_STAGE,diagnostic);
    if (!module->provenance || module->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE)
        return transition_error(XR_XIR_BAD_STRUCTURE,diagnostic);
    return artifact_verify_owned_effects(artifact,output,NULL,diagnostic);
}
