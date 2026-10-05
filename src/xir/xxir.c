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
#include "xxir_compile_memory.h"
#include "xxir_generic.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"

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

static XrXirArtifact *clone_module(const XrXirCompileContext *compile_context, const XrXirModule *source, XrXirStatus *allocation_status) {
    XrXirArtifact *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), allocation_status);
    if (!copy)
        return NULL;
    XrXirFunction *functions = xir_compile_calloc(compile_context, source->function_count, sizeof(*functions), allocation_status);
    if (!functions) {
        xr_compile_resources_free(copy);
        return NULL;
    }
    copy->context = *compile_context;
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
        const XrXirProvenance *from = source->provenance;
        XrXirProvenance *to = xir_compile_calloc(compile_context, 1, sizeof(*to), allocation_status);
        if (!to) { xr_xir_compile_artifact_free(copy); return NULL; }
        copy->module.provenance = to;
        to->origins = xir_compile_calloc(compile_context, from->count, sizeof(*to->origins), allocation_status);
        if (!to->origins) { xr_xir_compile_artifact_free(copy); return NULL; }
        to->count = from->count;
        to->source = clone_module(compile_context, &from->source->module, allocation_status);
        if (!to->source) { xr_xir_compile_artifact_free(copy); return NULL; }

        for (uint32_t i = 0; i < from->count; ++i) {
            to->origins[i] = from->origins[i];
            to->origins[i].arguments = copy_bytes(compile_context, from->origins[i].arguments,
                (size_t)from->origins[i].argument_count * sizeof(XrXirType), allocation_status);
            if (from->origins[i].argument_count && !to->origins[i].arguments) {
                xr_xir_compile_artifact_free(copy); return NULL;
            }
        }
    }
    return copy;
}

#include "xxir_provenance.inc.c"
#include "xxir_provenance_match.inc.c"
#include "xxir_types_lower.inc.c"
#include "xxir_checked_codec.inc.c"

static XrXirStatus transition_error(XrXirStatus status, XrXirDiagnostic *diagnostic) {
    if (diagnostic)
        *diagnostic = (XrXirDiagnostic) {status, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
    return status;
}

XrXirStatus xr_xir_compile_recheck(const XrXirCompileContext *compile_context, const XrXirModule *checked, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output) return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);

    if (!checked || checked->stage != XR_XIR_CHECKED) return transition_error(XR_XIR_BAD_STAGE, diagnostic);
    XrXirCompileContext limits = *budget;
    XrXirStatus status = xr_xir_compile_verify(&limits, checked, diagnostic);
    if (status != XR_XIR_OK) return status;
    XrXirArtifact *copy = clone_module(compile_context, checked, &allocation_status);
    if (!copy) return transition_error(allocation_status, diagnostic);
    copy->context = limits;
    status = xr_xir_compile_artifact_verify(copy, diagnostic);
    if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return status; }
    *output = copy; return XR_XIR_OK;
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

/* The selected private writer never outlives the caller's owned input proof. */
static XrXirStatus transition_owned(const XrXirModule *input,
    const XrXirCompileContext *budget, XrXirArtifact **output,
    XrXirDiagnostic *diagnostic, const XrXirTarget *target, TransitionPacketWriter writer) {
    XrXirStage source = input->stage;
    XrXirStatus allocation_status = XR_XIR_OK, status = XR_XIR_OK;
    XrXirCompileContext limits = *budget;
    XrXirArtifact *copy = clone_module(budget, input, &allocation_status);
    if (!copy)
        return transition_error(allocation_status, diagnostic);
    copy->context = limits;
    if (source == XR_XIR_CHECKED) {
        status = writer(copy, &copy->checked_packet, diagnostic);
        if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return status; }
        if (!xir_compile_work(budget, copy->checked_packet.length)) {
            xr_xir_compile_artifact_free(copy); return transition_error(XR_XIR_BUDGET, diagnostic);
        }
        xr_sha256(copy->checked_packet.bytes, copy->checked_packet.length, copy->checked_identity);
    }
    copy->module.stage = source == XR_XIR_BUILT ? XR_XIR_CHECKED : XR_XIR_LOWERED;
    if (copy->module.stage == XR_XIR_LOWERED) {
        XrXirCompileContext projection_budget = limits;
        status = lower_nominal_types(&copy->module, &projection_budget);
        if (status != XR_XIR_OK) { xr_xir_compile_artifact_free(copy); return transition_error(status, diagnostic); }
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
    status = xr_xir_compile_artifact_verify(copy, diagnostic);
    if (status != XR_XIR_OK) {
        xr_xir_compile_artifact_free(copy);
        return status;
    }
    *output = copy;
    return XR_XIR_OK;
}

static XrXirStatus transition(const XrXirModule *input, XrXirStage source,
    const XrXirCompileContext *budget, XrXirArtifact **output,
    XrXirDiagnostic *diagnostic, const XrXirTarget *target) {
    if (!xir_compile_context_valid(budget) || !output)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    XrXirStatus status = transition_shape(input, source, budget, target, diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_verify(budget, input, diagnostic);
    if (status != XR_XIR_OK) return status;
    return transition_owned(input, budget, output, diagnostic, target, xr_xir_compile_checked_write);
}

/* The reader owns and verifies the only input; no decoded view escapes. */
XR_FUNC XrXirStatus xr_xir_compile_checked_read_lower(const XrXirCompileContext *context,
    const void *bytes, size_t length, const XrXirTarget *target,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !output)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    XrXirArtifact *checked = NULL;
    XrXirStatus status = xr_xir_compile_checked_read(context, bytes, length, &checked, diagnostic);
    if (status == XR_XIR_OK)
        status = transition_shape(&checked->module, XR_XIR_CHECKED, &checked->context, target, diagnostic);
    if (status == XR_XIR_OK)
        status = transition_owned(&checked->module, &checked->context, output, diagnostic, target, checked_encode);
    xr_xir_compile_artifact_free(checked);
    return status;
}

XrXirStatus xr_xir_compile_check(const XrXirCompileContext *compile_context, const XrXirModule *built, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    return transition(built, XR_XIR_BUILT, budget, output, diagnostic, NULL);
}

XrXirStatus xr_xir_compile_lower(const XrXirArtifact *checked, const XrXirTarget *target, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!checked) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = checked->context;
    XrXirCompileContext *budget = &compile_state;
    return transition(xr_xir_compile_artifact_module(checked), XR_XIR_CHECKED, budget, output, diagnostic, target);
}

XrXirStatus xr_xir_compile_artifact_verify(const XrXirArtifact *artifact, XrXirDiagnostic *diagnostic) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
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
    }
    XrXirStatus status = xr_xir_compile_verify(&limits, xr_xir_compile_artifact_module(artifact), diagnostic);
    if (status != XR_XIR_OK)
        return status;
    status = xr_xir_compile_layout_verify(artifact);
    return status == XR_XIR_OK ? status : transition_error(status, diagnostic);
}
