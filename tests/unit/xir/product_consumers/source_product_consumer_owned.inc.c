/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_owned.inc.c - Current mixed backend reception
 */
typedef struct MixedProjection {
    XrXirResumeEntry resume;
    XrXirReleaseEntry release;
    uint64_t base_bytes;
    uint32_t count;
} MixedProjection;

static uint64_t mixed_layout_bytes(const XrXirFunctionLayout *layout) {
    return (uint64_t)layout->frame_bytes + (uint64_t)layout->outgoing_count * sizeof(XrXirValue) +
        (uint64_t)layout->path_count * sizeof(XrXirValuePathStep);
}

/* These scalar facts describe physical VM wiring. The common receiver proves
 * the current body, metadata and layouts independently for every reception. */
static XrXirStatus mixed_projection(const XrXirCompileContext *context, const XrXirArtifact *artifact,
    const MixedOwner *owner, uint32_t count, MixedProjection *projection) {
    if (!count)
        return XR_XIR_BAD_STRUCTURE;
    projection->count = count;
    projection->resume = owner->entries[0].resume;
    projection->release = owner->entries[0].release;
    const XrXirFunctionLayout *first = xr_xir_compile_artifact_layout(artifact, 0);
    if (!first || mixed_layout_bytes(first) > owner->entries[0].state_bytes)
        return XR_XIR_BAD_STRUCTURE;
    projection->base_bytes = owner->entries[0].state_bytes - mixed_layout_bytes(first);
    for (uint32_t f = 0; f < count; ++f) {
        if (!xir_compile_work(context, sizeof(XrXirCallEntry) + sizeof(XrXirVmBinding)))
            return XR_XIR_BUDGET;
        const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, f);
        if (!layout || owner->entries[f].resume != projection->resume ||
            owner->entries[f].release != projection->release ||
            owner->entries[f].state_bytes != projection->base_bytes + mixed_layout_bytes(layout))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* No allocating or releasing preparation operation follows this comparison.
 * Native rows come from the actual generated C fixture, never a saved PASS. */
static XrXirStatus mixed_match(const XrXirCompileContext *context, const XrXirArtifact *artifact,
    const MixedOwner *owner, const MixedProjection *projection, unsigned mode, const XrXirProgram *program) {
    if (!program || program->entry_count != projection->count || !program->permissions ||
        !program->permissions->input || program->permissions->input->artifact != artifact)
        return XR_XIR_BAD_STRUCTURE;
    XrCompileResourceStatus physical = xr_compile_resources_allocation(context->resources,
        owner->bindings, (uint64_t)projection->count * sizeof(*owner->bindings));
    if (physical != XR_COMPILE_RESOURCE_OK)
        return physical == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < projection->count; ++f) {
        if (!xir_compile_work(context, 2 * sizeof(XrXirCallEntry) + sizeof(XrXirVmBinding)))
            return XR_XIR_BUDGET;
        const XrXirCallEntry *entry = &program->entries[f];
        const XrXirVmBinding *binding = &owner->bindings[f];
        if (binding->artifact != artifact || binding->function != f)
            return XR_XIR_BAD_STRUCTURE;
        if ((f % 2 == 0) == (mode == 2)) {
            const XrXirCallEntry *native = &product_consumer_program.entries[f];
            if (entry->abi_version != native->abi_version || entry->resume != native->resume ||
                entry->release != native->release || entry->environment != native->environment ||
                entry->state_bytes != native->state_bytes)
                return XR_XIR_BAD_STRUCTURE;
        } else {
            const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(artifact, f);
            if (!layout || entry->abi_version != XR_XIR_CALL_ABI_VERSION ||
                entry->resume != projection->resume || entry->release != projection->release ||
                entry->environment != binding ||
                entry->state_bytes != projection->base_bytes + mixed_layout_bytes(layout))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus mixed_program_take(const XrXirCompileContext *context, XrXirArtifact **artifact,
    unsigned mode, XrXirProgram **output) {
    CHECK(artifact && *artifact && output && !*output);
    XrXirArtifact *original = *artifact;
    const XrXirModule *module = xr_xir_compile_artifact_module(original);
    MixedOwner *owner = NULL;
    XrXirProgramInput *input = NULL;
    XrXirProgram *program = NULL;
    MixedProjection projection = {0};
    XrXirStatus status = allocate(context, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_XIR_OK)
        return status;
    status = xr_xir_compile_vm_bind_table(original, &owner->bindings, &owner->entries);
    if (status == XR_XIR_OK)
        status = mixed_projection(context, original, owner, module->function_count, &projection);
    unsigned native = 0, vm = 0;
    if (status == XR_XIR_OK) {
        for (uint32_t f = 0; f < module->function_count; ++f) {
            if ((f % 2 == 0) == (mode == 2)) {
                owner->entries[f] = product_consumer_program.entries[f];
                ++native;
            } else {
                ++vm;
            }
        }
        CHECK(native && vm);
        XrXirProgramSpec spec = product_consumer_program;
        spec.abi_version = mixed_factory_abi;
        spec.entries = owner->entries;
        spec.declarations = module->declarations;
        spec.types = module->types;
        spec.packet = (XrXirProgramPacket){0};
        spec.code = (XrXirCodeLease){0};
        XrXirArtifact *owned = original;
        mixed_attack_bindings = mixed_binding_attack ? owner->bindings : NULL;
        status = xir_program_input_take(context, &owned, &input);
        if (status == XR_XIR_OK)
            status = xir_program_receive(context, &spec, &input, NULL, &program);
        mixed_attack_bindings = NULL;
    }
    if (status == XR_XIR_OK)
        status = mixed_match(context, original, owner, &projection, mode, program);
    if (status != XR_XIR_OK) {
        if (program) {
            program->permissions->input->artifact = NULL;
            xr_xir_compile_program_drop(program);
        } else if (input) {
            input->artifact = NULL;
            xir_program_input_free(input);
        }
        mixed_free(owner);
        CHECK(*artifact == original && !*output);
        return status;
    }
    CHECK(!input && *artifact == original && !*output);
    program->code = (XrXirCodeLease){owner, mixed_release};
    *artifact = NULL;
    *output = program;
    return XR_XIR_OK;
}
