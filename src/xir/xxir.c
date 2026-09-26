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
#include "xxir_callable.h"
#include "../base/xmalloc.h"

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
    xr_xir_generics_free((XrXirGeneric *) artifact->module.generics, artifact->module.function_count);
    xr_xir_declarations_free((XrXirDeclarations *) artifact->module.declarations);
    xr_xir_callable_types_free((XrXirCallableTypes *) artifact->module.callables);
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
    copy->module = (XrXirModule) {source->stage, functions, source->function_count, NULL, NULL, NULL};
    XrXirGeneric *generics = NULL;
    if (xr_xir_generics_clone(source, &generics) != XR_XIR_OK) {
        xr_xir_artifact_free(copy); return NULL;
    }
    copy->module.generics = generics;
    XrXirCallableTypes *callables = NULL;
    if (xr_xir_callable_types_clone(source->callables, &callables) != XR_XIR_OK) {
        xr_xir_artifact_free(copy); return NULL;
    }
    copy->module.callables = callables;
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
    return copy;
}

static XrXirStatus transition_error(XrXirStatus status, XrXirDiagnostic *diagnostic) {
    if (diagnostic)
        *diagnostic = (XrXirDiagnostic) {status, UINT32_MAX, UINT32_MAX, UINT32_MAX};
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
        if (input->generics || input->callables) return transition_error(XR_XIR_BAD_STAGE, diagnostic);
        XrXirLayout layout;
        if (xr_xir_layout(XR_XIR_I64, target, XR_XIR_LAYOUT_FRAME, &layout) != XR_XIR_OK)
            return transition_error(XR_XIR_BAD_LAYOUT, diagnostic);
    }
    XrXirStatus status = xr_xir_verify(input, budget, diagnostic);
    if (status != XR_XIR_OK)
        return status;
    XrXirArtifact *copy = clone_module(input);
    if (!copy)
        return transition_error(XR_XIR_OUT_OF_MEMORY, diagnostic);
    copy->budget = limits;
    copy->module.stage = source == XR_XIR_BUILT ? XR_XIR_CHECKED : XR_XIR_LOWERED;
    if (copy->module.stage == XR_XIR_LOWERED) {
        copy->target = *target;
        for (uint32_t f = 0; f < copy->module.function_count; ++f) {
            const XrXirFunction *function = &copy->module.functions[f];
            XrXirInstruction *instructions = (XrXirInstruction *) function->instructions;
            for (uint32_t i = 0; i < function->instruction_count; ++i) {
                if (instructions[i].op == XR_XIR_COPY)
                    instructions[i].op = xr_xir_type_is_owned(instructions[i].type) ?
                        XR_XIR_OWNED_RETAIN : XR_XIR_SCALAR_COPY;
                else if (instructions[i].op >= XR_XIR_LOCAL_NEW && instructions[i].op <= XR_XIR_LOCAL_WRITE) {
                    XrXirType type = instructions[i].op == XR_XIR_LOCAL_WRITE ?
                        instructions[instructions[i].args[0] - function->parameter_count].type : instructions[i].type;
                    instructions[i].op = (XrXirOp) ((xr_xir_type_is_owned(type) ? XR_XIR_OWNED_LOCAL_NEW :
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
    XrXirStatus status = xr_xir_verify(xr_xir_artifact_module(artifact), &limits, diagnostic);
    if (status != XR_XIR_OK)
        return status;
    status = xr_xir_layout_verify(artifact, &limits);
    return status == XR_XIR_OK ? status : transition_error(status, diagnostic);
}
