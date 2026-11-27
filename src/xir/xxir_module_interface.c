/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_module_interface.c - Fingerprint a closed subset of Source interfaces
 *
 * KEY CONCEPT: The live syntax supplies the complete export inventory. The
 * Checked owner supplies the admitted signatures and their observable effects.
 */
#include "xxir_module_interface.h"
#include "xxir_compile_memory.h"
#include "xxir_effect_module_export_internal.h"
#include "xxir_internal.h"
#include "xxir_receive_control.h"
#include "xxir_types.h"
#include "base/xsha256.h"
#include "frontend/parser/xast.h"
#include "module/xmodule_graph.h"
#include "toolchain/xcompiler_session.h"
#include <stdlib.h>
#include <string.h>

typedef struct ModuleInterface {
    const XrXirArtifact *checked;
    const XrXirDeclarations *declarations;
    const XrXirSlotFacts *slots;
    XirModuleEffectExportView effects;
    XrCompileRequestControl *control;
} ModuleInterface;

static void interface_integer(XrSHA256Context *hash, uint32_t value) {
    const uint8_t bytes[] = {(uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    xr_sha256_update(hash, bytes, sizeof(bytes));
}

static void interface_text(XrSHA256Context *hash, const char *bytes, uint32_t length) {
    interface_integer(hash, length);
    if (length) xr_sha256_update(hash, (const uint8_t *)bytes, length);
}

static int interface_digest_compare(const void *left, const void *right) {
    const XrFingerprint *a = left;
    const XrFingerprint *b = right;
    return memcmp(a->bytes, b->bytes, sizeof(a->bytes));
}

static void interface_set(XrSHA256Context *hash, XrFingerprint *entries, uint32_t count) {
    interface_integer(hash, count);
    if (!count) return;
    if (count > 1) qsort(entries, count, sizeof(*entries), interface_digest_compare);
    for (uint32_t i = 0; i < count; ++i)
        xr_sha256_update(hash, entries[i].bytes, sizeof(entries[i].bytes));
}

static bool interface_scalar(XrXirType type) {
    return type == XR_XIR_I64 || type == XR_XIR_BOOL;
}

static bool interface_declared_function(const AstNode *node) {
    if (node->type != AST_FUNCTION_DECL) return false;
    const FunctionDeclNode *function = &node->as.function_decl;
    if (function->is_extern || function->is_generator || function->attr_count ||
        function->type_param_count || function->borrow_origin_count ||
        function->throws_clause.syntax != XR_THROWS_OMITTED) return false;
    for (int i = 0; i < function->param_count; ++i) {
        const XrParamNode *parameter = function->params[i];
        if (!parameter || !parameter->name || !parameter->type || parameter->is_rest ||
            parameter->default_value || parameter->pattern || parameter->passing_mode != XR_PARAM_READ)
            return false;
    }
    return true;
}

/* Slot effects use names from this exact construction owner. Witness locations
 * and physical slot numbers can change with an implementation-only edit. */
static XrXirStatus interface_state_set(const ModuleInterface *interface, XirModuleEffectSpan span,
    XrSHA256Context *hash) {
    const XrXirDeclarations *declarations = interface->declarations;
    const XirModuleEffectExportView *effects = &interface->effects;
    if (span.first > effects->term_count || span.count > effects->term_count - span.first ||
        span.count > UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    XrFingerprint *entries = span.count ? xir_compile_calloc(&interface->checked->context,
        span.count, sizeof(*entries), &status) : NULL;
    if (span.count && !entries) return status;

    uint32_t progress = 0;
    for (size_t i = 0; i < span.count; ++i) {
        status = xir_receive_step(interface->control, &progress);
        if (status != XR_XIR_OK) break;
        const XirModuleEffectTerm *term = &effects->terms[span.first + i];
        if (term->kind != XIR_MODULE_EFFECT_SLOT) { status = XR_XIR_UNSUPPORTED; break; }
        if (term->identity >= declarations->slot_count) { status = XR_XIR_BAD_STRUCTURE; break; }
        const XrXirSlot *slot = &declarations->slots[term->identity];
        const XrXirSlotFact *name = &interface->slots->rows[term->identity];
        if (slot->module >= declarations->module_count || !name->name_length ||
            !name->name || name->slot != term->identity) { status = XR_XIR_BAD_STRUCTURE; break; }
        XrXirType type = slot->mutable ? xr_xir_cell_element(interface->checked->module.types, slot->type) : slot->type;
        if (!interface_scalar(type)) { status = XR_XIR_UNSUPPORTED; break; }
        const XrXirSourceModule *owner = &declarations->modules[slot->module];
        XrSHA256Context entry;
        xr_sha256_init(&entry);
        interface_text(&entry, owner->name, owner->name_length);
        interface_text(&entry, name->name, name->name_length);
        interface_integer(&entry, type);
        interface_integer(&entry, slot->mutable);
        xr_sha256_final(&entry, entries[i].bytes);
    }
    if (status == XR_XIR_OK) interface_set(hash, entries, (uint32_t)span.count);
    xr_compile_resources_free(entries);
    return status;
}

static XrXirStatus interface_effects(const ModuleInterface *interface, uint32_t function,
    XrSHA256Context *hash) {
    const XrXirEffects *owner = interface->checked->checked_effects;
    const XrXirEffectFacts facts = xr_xir_effects_read(owner, function);
    if (facts.status != XR_XIR_OK || function >= interface->effects.function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XirModuleEffectFunction *effects = &interface->effects.functions[function];
    if (effects->result_origin.count || effects->callback_loans.count) return XR_XIR_UNSUPPORTED;
    /* There can be no slot terms when the program has no module storage and
     * this projection admits neither reference parameters nor captured values. */
    if (!interface->effects.module_access_complete && interface->effects.slot_count)
        return XR_XIR_UNSUPPORTED;
    interface_integer(hash, facts.control.suspend);
    interface_integer(hash, facts.control.throws);
    interface_integer(hash, facts.root.requires_root);
    interface_integer(hash, facts.root.unresolved);
    interface_integer(hash, xr_xir_effects_task_creation(owner, function));
    interface_integer(hash, xr_xir_effects_go_safe(owner, function) == XR_XIR_OK);
    XrXirStatus status = interface_state_set(interface, effects->access, hash);
    return status != XR_XIR_OK ? status : interface_state_set(interface, effects->write, hash);
}

static XrXirStatus interface_function(const ModuleInterface *interface, uint32_t index,
    XrFingerprint *output) {
    const XrXirModule *module = &interface->checked->module;
    const XrXirFunctionIdentity *identity = &interface->declarations->functions[index];
    const XrXirFunction *function = &module->functions[index];
    if (identity->method_kind != XR_XIR_NON_MEMBER || identity->native_callable || identity->capture_count ||
        identity->cleanup_owner || identity->test_role || (identity->promises & ~XR_XIR_FUNCTION_NO_SUSPEND) ||
        function->consume_parameter_count || function->escaping_parameter_count ||
        function->error_type != XR_XIR_NEVER ||
        (function->result != XR_XIR_UNIT && !interface_scalar(function->result))) return XR_XIR_UNSUPPORTED;

    const XrXirParameterNames *labels = xr_xir_parameter_names_find(interface->declarations,
        XR_XIR_PARAMETER_FUNCTION, index, 0);
    if (!labels || labels->parameter_offset || labels->count != function->parameter_count ||
        (labels->count && !labels->names) || !function->name_length) return XR_XIR_BAD_STRUCTURE;
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    interface_text(&hash, function->name, function->name_length);
    interface_integer(&hash, identity->promises);
    interface_integer(&hash, labels->count);
    for (uint32_t p = 0; p < labels->count; ++p) {
        if (!interface_scalar(function->parameters[p]) || !labels->names[p].length)
            return XR_XIR_UNSUPPORTED;
        interface_text(&hash, labels->names[p].bytes, labels->names[p].length);
        interface_integer(&hash, function->parameters[p]);
    }
    interface_integer(&hash, function->result);
    XrXirStatus status = interface_effects(interface, index, &hash);
    if (status == XR_XIR_OK) xr_sha256_final(&hash, output->bytes);
    return status;
}

XR_FUNC XrXirStatus xir_module_interface_project(const XrModuleGraph *graph,
    const XrXirArtifact *checked, const char *canonical, XrFingerprint *output) {
    if (!graph || !checked || !canonical || !output || !checked->checked_effects ||
        checked->module.stage != XR_XIR_CHECKED || checked->context.resources != graph->resources)
        return XR_XIR_BAD_STRUCTURE;
    int ordinal = -1;
    XrModuleStatus found = xr_compile_module_graph_find(graph, canonical, &ordinal);
    if (found != XR_MODULE_OK) return XR_XIR_BAD_STRUCTURE;
    if (ordinal < 0) return XR_XIR_UNRESOLVED;
    const XrModuleSpec *spec = &graph->specs[ordinal];
    if (spec->kind != XR_MOD_FILE || spec->authority.kind != XR_MODULE_IDENTITY_SCRIPT ||
        !spec->ast || !spec->source_text || spec->ast->type != AST_PROGRAM) return XR_XIR_UNSUPPORTED;
    const XrXirDeclarations *declarations = checked->module.declarations;
    if (!declarations || (uint32_t)ordinal >= declarations->module_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *module = &declarations->modules[ordinal];
    if (module->name_length != strlen(spec->canonical) ||
        memcmp(module->name, spec->canonical, module->name_length)) return XR_XIR_BAD_STRUCTURE;
    const XrXirTarget *target = xr_xir_compile_artifact_target(checked);
    if (!xr_xir_target_is_host(target)) return XR_XIR_BAD_LAYOUT;

    XrCompileRequestControl *control = xr_compile_session_request(graph->compiler_session);
    XrXirStatus status = xir_receive_poll(control);
    if (status != XR_XIR_OK) return status;
    uint32_t progress = 0;
    uint32_t exports = 0;
    for (int i = 0; i < spec->ast->as.program.count; ++i) {
        status = xir_receive_step(control, &progress);
        if (status != XR_XIR_OK) return status;
        const AstNode *node = spec->ast->as.program.statements[i];
        if (!node->is_exported) continue;
        if (!interface_declared_function(node)) return XR_XIR_UNSUPPORTED;
        ++exports;
    }

    ModuleInterface interface = {.checked = checked, .declarations = declarations, .control = control,
        .slots = xr_xir_compile_construction_slots(checked->construction)};
    if (!interface.slots || interface.slots->count != declarations->slot_count) return XR_XIR_BAD_STRUCTURE;
    XirModuleEffectExport *effects = NULL;
    status = xir_effects_module_export_copy(checked, &effects);
    if (status != XR_XIR_OK) return status;
    status = xir_effects_module_export_view(effects, &interface.effects);
    XrFingerprint *entries = NULL;
    if (status == XR_XIR_OK && exports)
        entries = xir_compile_calloc(&checked->context, exports, sizeof(*entries), &status);

    static const uint8_t domain[] = "xray-source-scalar-interface-v1";
    XrSHA256Context hash;
    xr_sha256_init(&hash);
    xr_sha256_update(&hash, domain, sizeof(domain));
    interface_text(&hash, module->name, module->name_length);
    interface_integer(&hash, target->os);
    interface_integer(&hash, target->architecture);
    interface_integer(&hash, target->abi_version);
    if (status == XR_XIR_OK) status = interface_effects(&interface, module->initializer, &hash);
    uint32_t next = 0;
    for (uint32_t f = 0; status == XR_XIR_OK && f < checked->module.function_count; ++f) {
        status = xir_receive_step(control, &progress);
        if (status != XR_XIR_OK) break;
        const XrXirFunctionIdentity *identity = &declarations->functions[f];
        if (!identity->exported || identity->module != (uint32_t)ordinal) continue;
        if (next == exports) { status = XR_XIR_BAD_STRUCTURE; break; }
        status = interface_function(&interface, f, &entries[next++]);
    }
    if (status == XR_XIR_OK && next != exports) status = XR_XIR_BAD_STRUCTURE;
    if (status == XR_XIR_OK) {
        interface_set(&hash, entries, exports);
        status = xir_receive_poll(control);
        if (status == XR_XIR_OK) xr_sha256_final(&hash, output->bytes);
    }
    xr_compile_resources_free(entries);
    xir_effects_module_export_free(effects);
    return status;
}
