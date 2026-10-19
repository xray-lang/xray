/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program.c - Validated immutable program and code lease ownership
 *
 * KEY CONCEPT:
 *   Instances retain immutable descriptors until execution and cleanup finish.
 */

#include "xxir_program_internal.h"
#include "xxir_internal.h"
#include "../base/xsha256.h"
#include "xxir_compile_memory.h"
#include "xxir_atomic.h"
#include "../base/xchecks.h"

static bool program_value_type(const XrXirProgramSpec *spec, XrXirType type) {
    if (xr_xir_type_is_cell(spec->types, type)) {
        type = xr_xir_cell_element(spec->types, type);
        if (type == XR_XIR_UNIT) return true;
    }
    return type == XR_XIR_BOOL || type == XR_XIR_RUNE || xr_xir_type_is_number(type) || xr_xir_type_is_owned(spec->types, type);
}
static XrXirStatus program_shape(const XrXirCompileContext *context, const XrXirProgramSpec *spec) {
    if (!spec) return XR_XIR_BAD_STRUCTURE;
    if (spec->abi_version != XR_XIR_PROGRAM_ABI_VERSION ||
        spec->target.architecture != XR_XIR_ARCH_X86_64 || spec->target.abi_version != XR_XIR_VALUE_ABI_VERSION)
        return XR_XIR_BAD_LAYOUT;
    if (!spec->entries || !spec->entry_count || !spec->declarations || !spec->declarations->functions ||
        (!!spec->code.owner != !!spec->code.release)) return XR_XIR_BAD_STRUCTURE;
    if (spec->entry_count > 65535 || spec->entry_count > context->limits.functions) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (spec->entries[i].abi_version != XR_XIR_CALL_ABI_VERSION) return XR_XIR_BAD_LAYOUT;
        if ((spec->entries[i].flags & ~(XR_XIR_ENTRY_EXIT | XR_XIR_ENTRY_GENERATOR)) ||
            (((spec->entries[i].flags & XR_XIR_ENTRY_GENERATOR)!=0)!=
             ((spec->declarations->functions[i].promises & XR_XIR_FUNCTION_GENERATOR)!=0))) return XR_XIR_BAD_STRUCTURE;
    }
    if (spec->declarations->implementations || (spec->types && spec->types->interfaces)) return XR_XIR_BAD_STAGE;
    XrXirStatus status = xr_xir_compile_types_structure_verify(context, spec->types);
    if (status != XR_XIR_OK) return status;
    if (spec->types) for (uint32_t t = 0; t < spec->types->count; ++t) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        if (spec->types->nodes[t].parameter_span) return XR_XIR_BAD_TYPE;
    }
    status = xr_xir_compile_declarations_verify(context, spec->declarations, spec->types,
        spec->entry_count, XR_XIR_PROGRAM);
    if (status != XR_XIR_OK) return status;
    for (uint32_t s = 0; s < spec->declarations->slot_count; ++s) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirSlot *slot = &spec->declarations->slots[s];
        if (xr_xir_type_is_cell(spec->types, slot->type) != !!slot->mutable ||
            (slot->type != XR_XIR_UNIT && !program_value_type(spec, slot->type)) ||
            (!slot->mutable && xr_xir_type_is_callable(spec->types, slot->type) &&
             slot->module != spec->declarations->root_module)) return XR_XIR_BAD_TYPE;
    }
    for (uint32_t i = 0; i < spec->entry_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *entry = &spec->entries[i];
        if (spec->declarations->functions[i].test_role &&
            (entry->parameter_count || entry->result != XR_XIR_UNIT)) return XR_XIR_BAD_TYPE;
        bool generator=(spec->declarations->functions[i].promises & XR_XIR_FUNCTION_GENERATOR)!=0;
        const XrXirTypeNode *result=xr_xir_type_node(spec->types,entry->result);
        if (generator && (!result || result->kind!=XR_XIR_TYPE_ITERATOR || result->parameter_span)) return XR_XIR_BAD_TYPE;
        if ((entry->flags & ~(XR_XIR_ENTRY_EXIT | XR_XIR_ENTRY_GENERATOR)) ||
            (((entry->flags & XR_XIR_ENTRY_GENERATOR)!=0)!=generator) ||
            entry->cleanup_owner != spec->declarations->functions[i].cleanup_owner ||
            (entry->cleanup_owner && (entry->result != XR_XIR_UNIT ||
             !(spec->entries[entry->cleanup_owner - 1].flags & XR_XIR_ENTRY_EXIT)))) return XR_XIR_BAD_STRUCTURE;
        if (!entry->resume || (entry->parameter_count && !entry->parameters)) return XR_XIR_BAD_STRUCTURE;
        if (xr_xir_type_is_cell(spec->types, entry->result) ||
            (entry->result != XR_XIR_UNIT && !program_value_type(spec, entry->result))) return XR_XIR_BAD_TYPE;
        if (entry->parameter_count > context->limits.parameters) return XR_XIR_BUDGET;
        for (uint32_t p = 0; p < entry->parameter_count; ++p) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            /* A cell parameter is a ref parameter: only the program's own functions can build one. */
            if (!program_value_type(spec, entry->parameters[p])) return XR_XIR_BAD_TYPE;
        }
    }
    const XrXirDeclarations *d = spec->declarations;
    const XrXirCallEntry *entry = &spec->entries[d->entry_function];
    if (entry->parameter_count || entry->result != XR_XIR_I64) return XR_XIR_BAD_TYPE;
    for (uint32_t m = 0; m < d->module_count; ++m) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *init = &spec->entries[d->modules[m].initializer];
        if (init->parameter_count || init->result != XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
static void program_dispose(XrXirProgram *program) {
    if (program->code.release) program->code.release(program->code.owner);
    if (program->entries) for (uint32_t i = 0; i < program->entry_count; ++i)
        xr_compile_resources_free((void *)program->entries[i].parameters);
    xr_compile_resources_free(program->entries);
    xr_xir_compile_type_arena_drop(program->arena);
    xr_compile_resources_free(program->order);
    xr_compile_resources_free(program->active_modules);
    xr_compile_resources_free(program->module_slots);
    xir_program_permissions_free(program->permissions);
    xr_compile_resources_free(program);
}
static XrXirStatus program_execution_order(XrXirProgram *program) {
    const XrXirCompileContext *context = &program->context;
    const XrXirDeclarations *d = program->declarations;
    XrXirStatus status = xr_xir_compile_declarations_order(context, d, program->order);
    if (status != XR_XIR_OK) return status;
    program->active_modules[d->root_module] = 1;
    for (uint32_t i = d->module_count; i > 0; --i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        uint32_t id = program->order[i - 1];
        if (!program->active_modules[id]) continue;
        const XrXirSourceModule *module = &d->modules[id];
        for (uint32_t dep = 0; dep < module->dependency_count; ++dep) {
            if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
            program->active_modules[module->dependencies[dep]] = 1;
        }
    }
    for (uint32_t i = 0; i < d->module_count; ++i) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        uint32_t id = program->order[i];
        if (program->active_modules[id]) program->order[program->initialization_count++] = id;
    }
    return XR_XIR_OK;
}
static XrXirStatus program_copy_entries(XrXirProgram *program, const XrXirProgramSpec *spec) {
    const XrXirCompileContext *context = &program->context;
    XrXirStatus status = XR_XIR_OK;
    program->entries = xir_compile_calloc(context, spec->entry_count, sizeof(*program->entries), &status);
    for (uint32_t i = 0; i < spec->entry_count && status == XR_XIR_OK; ++i) {
        if (!xir_compile_work(context, sizeof(*program->entries))) return XR_XIR_BUDGET;
        program->entries[i] = spec->entries[i];
        program->entries[i].parameters = NULL;
        uint64_t bytes = (uint64_t)spec->entries[i].parameter_count * sizeof(XrXirType);
        if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
        program->entries[i].parameters = xir_compile_copy(context, spec->entries[i].parameters, (size_t)bytes, &status);
    }
    return status;
}
static XrXirStatus program_owned_storage(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, XrXirCallEntry *entries, const XrXirProgramInput *input,
    const XrXirArtifact *artifact) {
    if (!spec || !entries || spec->entries!=entries || !spec->entry_count ||
        (uintptr_t)entries%_Alignof(XrXirCallEntry)) return XR_XIR_BAD_STRUCTURE;
    if (spec->entry_count>65535 || spec->entry_count>context->limits.functions) return XR_XIR_BUDGET;
    uint64_t bytes=(uint64_t)spec->entry_count*sizeof(*entries);
    if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=xir_compile_resource_status(xr_compile_resources_allocation(
        context->resources,entries,(size_t)bytes));
    if (status!=XR_XIR_OK) return status;
    for (uint32_t i=0;i<spec->entry_count;++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirCallEntry *entry=&entries[i];
        if (!entry->parameter_count) {
            if (entry->parameters) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        if (entry->parameter_count>context->limits.parameters) return XR_XIR_BUDGET;
        if (!entry->parameters || (uintptr_t)entry->parameters%_Alignof(XrXirType) ||
            (const void *)entry->parameters==(const void *)entries ||
            (const void *)entry->parameters==(const void *)input ||
            (const void *)entry->parameters==(const void *)artifact) return XR_XIR_BAD_STRUCTURE;
        bytes=(uint64_t)entry->parameter_count*sizeof(*entry->parameters);
        if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
        status=xir_compile_resource_status(xr_compile_resources_allocation(
            context->resources,entry->parameters,(size_t)bytes));
        if (status!=XR_XIR_OK) return status;
        for (uint32_t p=0;p<i;++p) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (entries[p].parameters==entry->parameters) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_program_receive(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec,XrXirProgramInput **input,
    XrXirCallEntry **owned_entries,XrXirProgram **output) {
    if (!output || *output || !input || !*input || !xir_compile_context_valid(context)) return XR_XIR_BAD_STRUCTURE;
    XrXirProgramInput *original_input=*input;
    if ((uintptr_t)original_input%_Alignof(XrXirProgramInput)) return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus input_extent=xr_compile_resources_allocation(context->resources,
        original_input,sizeof(*original_input));
    if (input_extent!=XR_COMPILE_RESOURCE_OK)
        return input_extent==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrXirArtifact *original_artifact=original_input->artifact;
    if (!original_artifact || (uintptr_t)original_artifact%_Alignof(XrXirArtifact)) return XR_XIR_BAD_STRUCTURE;
    input_extent=xr_compile_resources_allocation(context->resources,original_artifact,sizeof(*original_artifact));
    if (input_extent!=XR_COMPILE_RESOURCE_OK)
        return input_extent==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=owned_entries?program_owned_storage(context,spec,*owned_entries,
        original_input,original_artifact):XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    status = program_shape(context, spec);
    if (status != XR_XIR_OK) return status;
    const XrXirProgramSpec physical = *spec;
    const XrXirCodeLease code = physical.code;
    const XrXirProgramPacket packet=physical.packet;
    XrXirProgramPermissions *permissions = NULL;
    status = xir_program_input_prepare(context,&physical,*input,&permissions);
    if (status != XR_XIR_OK) return status;
    const XrXirTypes *types = NULL;
    const XrXirDeclarations *declarations = NULL;
    status = xir_effects_program_metadata(context, permissions->effects, &types, &declarations);
    if (status != XR_XIR_OK) { xir_program_permissions_free(permissions); return status; }
    /* The received Input owns canonical semantic storage. The physical callback
     * wiring remains borrowed until the final current-descriptor comparison. */
    for (uint32_t i = 0; i < types->count; ++i) {
        if (!xir_compile_work(context, 1)) { xir_program_permissions_free(permissions); return XR_XIR_BUDGET; }
        if (types->nodes[i].kind == XR_XIR_TYPE_ATOMIC && !xr_xir_atomic_capability())
            { xir_program_permissions_free(permissions); return XR_XIR_UNSUPPORTED; }
    }
    XrXirProgram *program = xir_compile_calloc(context, 1, sizeof(*program), &status);
    if (!program) { xir_program_permissions_free(permissions); return status; }
    program->permissions = permissions;
    atomic_init(&program->references, 1);
    program->context = *context;
    program->entry_count = permissions->function_count;
    if (owned_entries) {
        status=program_owned_storage(context,&physical,*owned_entries,original_input,original_artifact);
        if (status!=XR_XIR_OK) goto failed;
        /* The same current descriptor proof checks both copied and moved rows.
         * This slot transfers physical storage only; it grants no permission. */
        program->entries=*owned_entries;
    } else status = program_copy_entries(program, &physical);
    program->order = xir_compile_calloc(context, declarations->module_count, sizeof(*program->order), &status);
    program->active_modules = xir_compile_calloc(context, declarations->module_count, 1, &status);
    program->module_slots = xir_compile_calloc(context, declarations->module_count, sizeof(*program->module_slots), &status);
    if (status != XR_XIR_OK) goto failed;
    if (physical.types) {
        XrXirValueStatus value_status = xr_xir_compile_type_arena_new(context, types, &program->arena);
        if (value_status != XR_XIR_VALUE_OK) {
            status = value_status == XR_XIR_VALUE_OOM ? XR_XIR_OUT_OF_MEMORY :
                value_status == XR_XIR_VALUE_BAD_ARGUMENT ? XR_XIR_BAD_TYPE :
                value_status == XR_XIR_VALUE_UNSUPPORTED ? XR_XIR_UNSUPPORTED : XR_XIR_BUDGET;
            goto failed;
        }
        program->types = xr_xir_compile_type_arena_types(program->arena);
    }
    /* The received Input owns these immutable declarations for the complete
     * Program lifetime, including every Instance and retained code lease. */
    program->declarations = declarations;
    status = program_execution_order(program);
    if (status != XR_XIR_OK) goto failed;
    for (uint32_t i = 0; i < program->declarations->slot_count; ++i) {
        if (!xir_compile_work(context, 1)) { status = XR_XIR_BUDGET; goto failed; }
        ++program->module_slots[program->declarations->slots[i].module];
    }
    XrXirProgramSpec owned={XR_XIR_PROGRAM_ABI_VERSION,permissions->target,
        program->entries,program->entry_count,program->declarations,{0},program->types,{0}};
    /* Every preparation callback has finished. The independently checked
     * canonical fields also prove the runtime descriptors' structural shape. */
    if (*output || spec->code.owner!=code.owner || spec->code.release!=code.release ||
        *input!=original_input || spec->packet.bytes!=packet.bytes || spec->packet.length!=packet.length ||
        spec->packet.identity!=packet.identity) {
        status=XR_XIR_BAD_STRUCTURE;goto failed;
    }
    status=xir_program_input_verify(context,*input);
    if (status==XR_XIR_OK && (original_input->artifact!=original_artifact ||
        original_artifact->program_final!=permissions->effects || original_artifact->layouts!=permissions->layouts))
        status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK) status=xr_xir_compile_program_body_match(context,permissions,original_artifact);
    if (status==XR_XIR_OK && owned_entries) {
        if (spec->entries!=physical.entries || spec->entry_count!=physical.entry_count)
            status=XR_XIR_BAD_STRUCTURE;
        else status=program_owned_storage(context,&physical,*owned_entries,original_input,original_artifact);
    }
    if (status==XR_XIR_OK) status=xr_xir_compile_program_descriptor_match(context,spec,permissions);
    if (status==XR_XIR_OK) status=xr_xir_compile_program_descriptor_match(context,&owned,permissions);
    if (status==XR_XIR_OK && (packet.bytes || packet.length || packet.identity)) {
        if (!packet.bytes || !packet.identity) status=XR_XIR_BAD_STRUCTURE;
        else if (packet.length>UINT64_MAX-32 || !xir_compile_work(context,packet.length+32)) status=XR_XIR_BUDGET;
        else {
            uint8_t digest[32];
            xr_sha256(packet.bytes,packet.length,digest);
            if (memcmp(digest,permissions->packet_identity,32) || memcmp(packet.identity,digest,32))
                status=XR_XIR_BAD_STRUCTURE;
        }
    }
    if (status!=XR_XIR_OK) goto failed;
    permissions->input=*input;*input=NULL;
    if (owned_entries) *owned_entries=NULL;
    program->code = code;
    *output = program;
    return XR_XIR_OK;
failed:
    /* A provisional descriptor borrows the caller's physical table. Only
     * successful publication moves that owner; failure leaves it live. */
    if (owned_entries) program->entries=NULL;
    program_dispose(program);
    return status;
}
XR_FUNC XrXirStatus xr_xir_compile_program_seal(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec,XrXirProgram **output) {
    if (!xir_compile_context_valid(context) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=program_shape(context,spec);
    if (status!=XR_XIR_OK) return status;
    XrXirProgramInput *input=NULL;
    status=xir_program_input_read(context,&spec->packet,&spec->target,&input);
    if (status==XR_XIR_OK) status=xir_program_receive(context,spec,&input,NULL,output);
    xir_program_input_free(input);return status;
}
XR_FUNC bool xr_xir_compile_program_retain(XrXirProgram *program) {
    if (!program) return false;
    uint32_t count = atomic_load_explicit(&program->references, memory_order_relaxed);
    for (;;) {
        if (!count || count == UINT32_MAX) return false;
        if (atomic_compare_exchange_weak_explicit(&program->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
}
XR_FUNC void xr_xir_compile_program_drop(XrXirProgram *program) {
    if (!program) return;
    uint32_t count = atomic_load_explicit(&program->references, memory_order_relaxed);
    for (;;) {
        XR_CHECK(count, "program reference underflow");
        if (atomic_compare_exchange_weak_explicit(&program->references, &count, count - 1,
                memory_order_acq_rel, memory_order_relaxed)) break;
    }
    if (count == 1) program_dispose(program);
}
