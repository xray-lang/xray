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
#include "xxir_generic.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"

XrXirBudget xr_xir_default_budget(void) {
    return (XrXirBudget) {1024, 65536, 4096, 1048576,
                         UINT64_C(128) * 1024 * 1024,
                         UINT64_C(16) * 1024 * 1024, UINT64_C(16000000),
                         UINT64_C(16) * 1024 * 1024};
}

const char *xr_xir_op_name(XrXirOp op) {
    switch (op) {
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) \
    case XR_XIR_##name: return #name;
#include "xxir_ops.def"
#undef XR_XIR_OP
    default: return NULL;
    }
}

const XrXirModule *xr_xir_artifact_module(const XrXirArtifact *artifact) {
    return artifact ? &artifact->module : NULL;
}

const XrXirTarget *xr_xir_artifact_target(const XrXirArtifact *artifact) {
    return artifact && artifact->module.stage == XR_XIR_LOWERED ? &artifact->target : NULL;
}

const XrXirFunctionLayout *xr_xir_artifact_layout(const XrXirArtifact *artifact, uint32_t function) {
    return artifact && artifact->layouts && function < artifact->module.function_count ?
        &artifact->layouts[function] : NULL;
}

void xr_xir_artifact_free(XrXirArtifact *artifact) {
    if (!artifact)
        return;
    XrXirFunction *functions = (XrXirFunction *) artifact->module.functions;
    for (uint32_t i = 0; i < artifact->module.function_count; ++i) {
        xr_free((void *) functions[i].name);
        xr_free((void *) functions[i].parameters);
        xr_free((void *) functions[i].blocks);
        xr_free((void *) functions[i].instructions);
        xr_free((void *) functions[i].operands);
        if (artifact->layouts) {
            xr_free((void *) artifact->layouts[i].offsets);
            xr_free((void *) artifact->layouts[i].parameters);
            xr_free((void *) artifact->layouts[i].owned_offsets);
        }
    }
    xr_free(artifact->layouts);
    xr_xir_checked_packet_free(&artifact->checked_packet);
    xr_xir_generics_free((XrXirGeneric *) artifact->module.generics, artifact->module.function_count);
    xr_xir_declarations_free((XrXirDeclarations *) artifact->module.declarations);
    xr_xir_types_free((XrXirTypes *) artifact->module.types);
    xr_xir_provenance_free((XrXirProvenance *)artifact->module.provenance);
    xr_free(functions);
    xr_free(artifact);
}

static void *copy_bytes(const void *source, size_t size) {
    if (!size)
        return NULL;
    void *copy = xr_malloc(size);
    if (copy)
        memcpy(copy, source, size);
    return copy;
}

static XrXirArtifact *clone_module(const XrXirModule *source) {
    XrXirArtifact *copy = xr_calloc(1, sizeof(*copy));
    if (!copy)
        return NULL;
    XrXirFunction *functions = xr_calloc(source->function_count, sizeof(*functions));
    if (!functions) {
        xr_free(copy);
        return NULL;
    }
    copy->module = (XrXirModule) {source->stage, functions, source->function_count, NULL, NULL, NULL, NULL};
    XrXirGeneric *generics = NULL;
    if (xr_xir_generics_clone(source, &generics) != XR_XIR_OK) {
        xr_xir_artifact_free(copy); return NULL;
    }
    copy->module.generics = generics;
    XrXirTypes *types = NULL;
    if (xr_xir_types_clone(source->types, &types) != XR_XIR_OK) {
        xr_xir_artifact_free(copy); return NULL;
    }
    copy->module.types = types;
    XrXirDeclarations *declarations = NULL;
    if (xr_xir_declarations_clone(source->declarations, source->function_count, &declarations) != XR_XIR_OK) {
        xr_xir_artifact_free(copy);
        return NULL;
    }
    copy->module.declarations = declarations;
    for (uint32_t i = 0; i < source->function_count; ++i) {
        const XrXirFunction *from = &source->functions[i];
        XrXirFunction *to = &functions[i];
        *to = *from;
        to->name = copy_bytes(from->name, from->name_length);
        to->parameters = copy_bytes(from->parameters,
                                   (size_t) from->parameter_count * sizeof(*from->parameters));
        to->blocks = copy_bytes(from->blocks, (size_t) from->block_count * sizeof(*from->blocks));
        to->instructions = copy_bytes(from->instructions,
                                     (size_t) from->instruction_count * sizeof(*from->instructions));
        to->operands = copy_bytes(from->operands, (size_t) from->operand_count * sizeof(*from->operands));
        if (!to->name || (to->parameter_count && !to->parameters) ||
            !to->blocks || !to->instructions || (to->operand_count && !to->operands)) {
            xr_xir_artifact_free(copy);
            return NULL;
        }
    }
    if (source->provenance) {
        const XrXirProvenance *from = source->provenance;
        XrXirProvenance *to = xr_calloc(1, sizeof(*to));
        if (!to) { xr_xir_artifact_free(copy); return NULL; }
        copy->module.provenance = to;
        to->origins = xr_calloc(from->count, sizeof(*to->origins));
        if (!to->origins) { xr_xir_artifact_free(copy); return NULL; }
        to->count = from->count;
        to->source = clone_module(&from->source->module);
        if (!to->source) { xr_xir_artifact_free(copy); return NULL; }
        to->source->budget = from->source->budget;
        for (uint32_t i = 0; i < from->count; ++i) {
            to->origins[i] = from->origins[i];
            to->origins[i].arguments = copy_bytes(from->origins[i].arguments,
                (size_t)from->origins[i].argument_count * sizeof(XrXirType));
            if (from->origins[i].argument_count && !to->origins[i].arguments) {
                xr_xir_artifact_free(copy); return NULL;
            }
        }
    }
    return copy;
}

#include "xxir_provenance.inc.c"
#include "xxir_provenance_match.inc.c"
#include "xxir_types_lower.inc.c"

static XrXirStatus transition_error(XrXirStatus status, XrXirDiagnostic *diagnostic) {
    if (diagnostic)
        *diagnostic = (XrXirDiagnostic) {status, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
    return status;
}

XrXirStatus xr_xir_recheck(const XrXirModule *checked, const XrXirBudget *budget,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (!output) return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    *output = NULL;
    if (!checked || checked->stage != XR_XIR_CHECKED) return transition_error(XR_XIR_BAD_STAGE, diagnostic);
    XrXirBudget limits = budget ? *budget : xr_xir_default_budget();
    XrXirStatus status = xr_xir_verify(checked, &limits, diagnostic);
    if (status != XR_XIR_OK) return status;
    XrXirArtifact *copy = clone_module(checked);
    if (!copy) return transition_error(XR_XIR_OUT_OF_MEMORY, diagnostic);
    copy->budget = limits;
    status = xr_xir_artifact_verify(copy, &limits, diagnostic);
    if (status != XR_XIR_OK) { xr_xir_artifact_free(copy); return status; }
    *output = copy; return XR_XIR_OK;
}

static XrXirStatus transition(const XrXirModule *input, XrXirStage source,
                             const XrXirBudget *budget, XrXirArtifact **output,
                             XrXirDiagnostic *diagnostic, const XrXirTarget *target) {
    if (!output)
        return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    *output = NULL;
    if (!input || input->stage != source)
        return transition_error(XR_XIR_BAD_STAGE, diagnostic);
    XrXirBudget limits = budget ? *budget : xr_xir_default_budget();
    if (source == XR_XIR_CHECKED) {
        if (input->generics || (input->types && input->types->interfaces))
            return transition_error(XR_XIR_BAD_STAGE, diagnostic);
        XrXirLayout layout;
        if (xr_xir_layout(NULL, XR_XIR_I64, target, XR_XIR_LAYOUT_FRAME, &layout) != XR_XIR_OK)
            return transition_error(XR_XIR_BAD_LAYOUT, diagnostic);
    }
    XrXirStatus status = xr_xir_verify(input, budget, diagnostic);
    if (status != XR_XIR_OK)
        return status;
    XrXirArtifact *copy = clone_module(input);
    if (!copy)
        return transition_error(XR_XIR_OUT_OF_MEMORY, diagnostic);
    copy->budget = limits;
    if (source == XR_XIR_CHECKED) {
        status = xr_xir_checked_write(copy, &limits, &copy->checked_packet, diagnostic);
        if (status != XR_XIR_OK) { xr_xir_artifact_free(copy); return status; }
        xr_sha256(copy->checked_packet.bytes, copy->checked_packet.length, copy->checked_identity);
    }
    copy->module.stage = source == XR_XIR_BUILT ? XR_XIR_CHECKED : XR_XIR_LOWERED;
    if (copy->module.stage == XR_XIR_LOWERED) {
        XrXirBudget projection_budget = limits;
        status = lower_nominal_types(&copy->module, &projection_budget);
        if (status != XR_XIR_OK) { xr_xir_artifact_free(copy); return transition_error(status, diagnostic); }
        copy->target = *target;
        for (uint32_t f = 0; f < copy->module.function_count; ++f) {
            const XrXirFunction *function = &copy->module.functions[f];
            XrXirInstruction *instructions = (XrXirInstruction *) function->instructions;
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
        status = xr_xir_layout_build(copy, &limits);
        if (status != XR_XIR_OK) {
            xr_xir_artifact_free(copy);
            return transition_error(status, diagnostic);
        }
    }
    status = xr_xir_artifact_verify(copy, budget, diagnostic);
    if (status != XR_XIR_OK) {
        xr_xir_artifact_free(copy);
        return status;
    }
    *output = copy;
    return XR_XIR_OK;
}

XrXirStatus xr_xir_check(const XrXirModule *built, const XrXirBudget *budget,
                       XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    return transition(built, XR_XIR_BUILT, budget, output, diagnostic, NULL);
}

XrXirStatus xr_xir_lower(const XrXirArtifact *checked, const XrXirTarget *target,
                       const XrXirBudget *budget,
                       XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    return transition(xr_xir_artifact_module(checked), XR_XIR_CHECKED, budget, output, diagnostic, target);
}

XrXirStatus xr_xir_artifact_verify(const XrXirArtifact *artifact, const XrXirBudget *budget,
                                 XrXirDiagnostic *diagnostic) {
    XrXirBudget limits = budget ? *budget : (artifact ? artifact->budget : xr_xir_default_budget());
    if (artifact && artifact->module.stage == XR_XIR_LOWERED) {
        const XrXirCheckedPacket *packet = &artifact->checked_packet;
        if (!packet->bytes || packet->length < 64)
            return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
        if (packet->length > limits.metadata_bytes || packet->length > limits.work)
            return transition_error(XR_XIR_BUDGET, diagnostic);
        limits.metadata_bytes -= packet->length;
        limits.work -= packet->length;
        uint8_t digest[32];
        xr_sha256(packet->bytes, packet->length, digest);
        if (memcmp(digest, artifact->checked_identity, sizeof(digest)))
            return transition_error(XR_XIR_BAD_STRUCTURE, diagnostic);
    }
    XrXirStatus status = xr_xir_verify(xr_xir_artifact_module(artifact), &limits, diagnostic);
    if (status != XR_XIR_OK)
        return status;
    status = xr_xir_layout_verify(artifact, &limits);
    return status == XR_XIR_OK ? status : transition_error(status, diagnostic);
}
