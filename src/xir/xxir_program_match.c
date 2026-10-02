/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program_match.c - Bounded correspondence of runtime descriptors
 *
 * KEY CONCEPT:
 *   Descriptor equality proves metadata correspondence, not callback behavior.
 */
#include "xxir_program_internal.h"
#include "xxir_checked.h"
#include "xxir_internal.h"
#include "xxir_compile_memory.h"
#include "../base/xsha256.h"

#define MATCH(a, b) do { \
    if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET; if ((a) != (b)) return XR_XIR_BAD_STRUCTURE; \
} while (0)
#define TRY(expression) do { \
    XrXirStatus status = (expression); \
    if (status != XR_XIR_OK) return status; \
} while (0)
static XrXirStatus match_bytes(const void *a, const void *b, size_t size, const XrXirCompileContext *context) {
    if (!xir_compile_work(context, size)) return XR_XIR_BUDGET;
    return size && (!a || !b || memcmp(a, b, size)) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus match_literal(XrXirLiteral a, XrXirLiteral b, const XrXirCompileContext *context) {
    MATCH(a.length, b.length);
    return match_bytes(a.bytes, b.bytes, a.length, context);
}
static XrXirStatus match_types(const XrXirTypes *a, const XrXirTypes *b, const XrXirCompileContext *context) {
    MATCH(!!a, !!b);
    if (!a) return XR_XIR_OK;
    MATCH(a->count, b->count);
    for (uint32_t i = 0; i < a->count; ++i) {
        const XrXirTypeNode *x = &a->nodes[i], *y = &b->nodes[i];
        MATCH(x->kind, y->kind); MATCH(x->element, y->element);
        MATCH(x->parameter_count, y->parameter_count); MATCH(x->result, y->result);
        MATCH(x->flags, y->flags); MATCH(x->parameter_span, y->parameter_span);
        for (uint32_t p = 0; p < x->parameter_count; ++p) {
            MATCH(x->parameters[p].type, y->parameters[p].type);
            MATCH(x->parameters[p].mode, y->parameters[p].mode);
        }
        MATCH(x->nominal.declaration, y->nominal.declaration);
        MATCH(x->nominal.argument_count, y->nominal.argument_count);
        MATCH(x->nominal.field_count, y->nominal.field_count);
        for (uint32_t p = 0; p < x->nominal.argument_count; ++p)
            MATCH(x->nominal.arguments[p], y->nominal.arguments[p]);
        for (uint32_t p = 0; p < x->nominal.field_count; ++p)
            MATCH(x->nominal.fields[p], y->nominal.fields[p]);
    }
    const XrXirNominalTable *x = a->nominals, *y = b->nominals;
    MATCH(!!x, !!y);
    if (!x) return XR_XIR_OK;
    MATCH(x->count, y->count);
    if (x->declarations || y->declarations || !x->identities || !y->identities)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < x->count; ++i) {
        const XrXirNominalIdentity *u = &x->identities[i], *v = &y->identities[i];
        TRY(match_literal(u->module, v->module, context));
        TRY(match_literal(u->name, v->name, context));
        MATCH(u->exported, v->exported); MATCH(u->arity, v->arity);
        MATCH(u->kind, v->kind); MATCH(u->flags, v->flags); MATCH(u->variant_count, v->variant_count);
        for (uint32_t j = 0; j < u->variant_count; ++j) {
            TRY(match_literal(u->variants[j].name, v->variants[j].name, context));
            MATCH(u->variants[j].field_begin, v->variants[j].field_begin);
            MATCH(u->variants[j].field_count, v->variants[j].field_count);
        }
        MATCH(u->field_count, v->field_count);
        for (uint32_t p = 0; p < u->field_count; ++p) {
            TRY(match_literal(u->fields[p].name, v->fields[p].name, context));
            MATCH(u->fields[p].flags, v->fields[p].flags);
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus match_declarations(const XrXirDeclarations *a,
    const XrXirDeclarations *b, uint32_t functions, const XrXirCompileContext *context) {
    if (!a || !b) return XR_XIR_BAD_STRUCTURE;
    MATCH(a->module_count, b->module_count); MATCH(a->slot_count, b->slot_count);
    MATCH(a->literal_count, b->literal_count); MATCH(a->root_module, b->root_module);
    MATCH(a->entry_function, b->entry_function);
    for (uint32_t i = 0; i < a->module_count; ++i) {
        const XrXirSourceModule *x = &a->modules[i], *y = &b->modules[i];
        MATCH(x->name_length, y->name_length);
        TRY(match_bytes(x->name, y->name, x->name_length, context));
        MATCH(x->initializer, y->initializer); MATCH(x->dependency_count, y->dependency_count);
        for (uint32_t p = 0; p < x->dependency_count; ++p)
            MATCH(x->dependencies[p], y->dependencies[p]);
    }
    for (uint32_t i = 0; i < functions; ++i) {
        MATCH(a->functions[i].module, b->functions[i].module);
        MATCH(a->functions[i].exported, b->functions[i].exported);
        MATCH(a->functions[i].nominal_owner, b->functions[i].nominal_owner);
        MATCH(a->functions[i].member_access, b->functions[i].member_access);
        MATCH(a->functions[i].cleanup_owner, b->functions[i].cleanup_owner);
        MATCH(a->functions[i].promises, b->functions[i].promises);
        MATCH(a->functions[i].method_kind, b->functions[i].method_kind);
    }
    for (uint32_t i = 0; i < a->slot_count; ++i) {
        MATCH(a->slots[i].module, b->slots[i].module);
        MATCH(a->slots[i].type, b->slots[i].type);
        MATCH(a->slots[i].mutable, b->slots[i].mutable);
    }
    for (uint32_t i = 0; i < a->literal_count; ++i)
        TRY(match_literal(a->literals[i], b->literals[i], context));
    return XR_XIR_OK;
}
static XrXirStatus match_layout(const XrXirFunctionLayout *a,
    const XrXirFunctionLayout *b, uint32_t parameters, const XrXirCompileContext *context) {
    MATCH(a->slot_count, b->slot_count); MATCH(a->frame_bytes, b->frame_bytes);
    MATCH(a->owned_count, b->owned_count); MATCH(a->outgoing_count, b->outgoing_count);
    MATCH(a->path_count, b->path_count);
    MATCH(a->result.size, b->result.size); MATCH(a->result.alignment, b->result.alignment);
    if ((a->slot_count && !a->offsets) || (a->owned_count && !a->owned_offsets) ||
        (parameters && !a->parameters) || (!parameters && a->parameters))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < a->slot_count; ++i)
        MATCH(a->offsets[i], b->offsets[i]);
    for (uint32_t i = 0; i < a->owned_count; ++i)
        MATCH(a->owned_offsets[i], b->owned_offsets[i]);
    for (uint32_t i = 0; i < parameters; ++i) {
        MATCH(a->parameters[i].size, b->parameters[i].size);
        MATCH(a->parameters[i].alignment, b->parameters[i].alignment);
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_program_match(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirFunctionLayout *layouts, const XrXirArtifact *lowered) {
    if (!spec || !layouts || !lowered || !xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    const XrXirCompileContext *owner_context = xr_xir_compile_artifact_context(lowered);
    if (!owner_context || owner_context->resources != context->resources) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const XrXirTarget *target = xr_xir_compile_artifact_target(lowered);
    if (!target) return XR_XIR_BAD_STAGE;
    MATCH(spec->target.architecture, target->architecture);
    MATCH(spec->target.abi_version, target->abi_version);
    MATCH(spec->entry_count, module->function_count);
    if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
    bool cleanup = false;
    for (uint32_t i = 0; i < module->function_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (module->declarations->functions[i].cleanup_owner) cleanup = true;
    }
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        const XrXirCallEntry *entry = &spec->entries[i];
        uint32_t flags = 0;
        if (cleanup) for (uint32_t child = 0; child < module->function_count; ++child) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            if (module->declarations->functions[child].cleanup_owner == i + 1) flags = XR_XIR_ENTRY_EXIT;
        }
        MATCH(entry->flags, flags);
        MATCH(entry->cleanup_owner, module->declarations->functions[i].cleanup_owner);
        const XrXirFunction *function = &module->functions[i];
        MATCH(entry->parameter_count, function->parameter_count);
        MATCH(entry->result, function->result);
        TRY(match_layout(&layouts[i], xr_xir_compile_artifact_layout(lowered, i),
            function->parameter_count, context));
        for (uint32_t p = 0; p < entry->parameter_count; ++p)
            MATCH(entry->parameters[p], function->parameters[p]);
    }
    TRY(match_types(spec->types, module->types, context));
    return match_declarations(spec->declarations, module->declarations, spec->entry_count, context);
}
#undef MATCH
#undef TRY

XR_FUNC XrXirStatus xr_xir_compile_program_proof_verify(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirProgramProof *proof) {
    if (!xir_compile_context_valid(context) || !spec || !proof || !proof->bytes ||
        proof->length < 64 || !proof->identity || !proof->layouts) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context, proof->length)) return XR_XIR_BUDGET;
    uint8_t digest[32];
    xr_sha256(proof->bytes, proof->length, digest);
    if (!xir_compile_work(context, sizeof(digest))) return XR_XIR_BUDGET;
    if (memcmp(digest, proof->identity, sizeof(digest))) return XR_XIR_BAD_STRUCTURE;
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirStatus status = xr_xir_compile_checked_read(context, proof->bytes, proof->length, &checked, NULL);
    if (status == XR_XIR_OK)
        status = xr_xir_compile_lower(checked, &spec->target, &lowered, NULL);
    xr_xir_compile_artifact_free(checked);
    if (status == XR_XIR_OK)
        status = xr_xir_compile_program_match(context, spec, proof->layouts, lowered);
    xr_xir_compile_artifact_free(lowered);
    return status;
}

XR_FUNC XrXirProgramProof xr_xir_compile_program_proof(const XrXirArtifact *artifact) {
    if (!artifact || artifact->module.stage != XR_XIR_LOWERED)
        return (XrXirProgramProof) {0};
    return (XrXirProgramProof) {artifact->checked_packet.bytes, artifact->checked_packet.length,
        artifact->checked_identity, artifact->layouts};
}
