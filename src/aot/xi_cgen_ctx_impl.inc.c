/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_ctx_impl.inc.c - AOT codegen context accessors
 */

XR_FUNC XiCgenCtx *xi_cgen_ctx_new(void) {
    XiCgenCtx *ctx = (XiCgenCtx *) xr_calloc(1, sizeof(XiCgenCtx));
    if (!ctx)
        return NULL;
    ctx->shared_name = "xrt_shared";
    ctx->emit_main = true;
    ctx->type_name_profile = XI_CGEN_TYPE_NAMES_ALL;
    /* Allocate the grow-on-demand shared-slot / method / import tables at
     * their initial capacity (cg_reserve_* grow them for large modules). */
    ctx->shared_funcs = (const XiFunc **) xr_calloc(CG_INIT_SHARED, sizeof(const XiFunc *));
    ctx->shared_class =
        (const XiClassData **) xr_calloc(CG_INIT_SHARED, sizeof(const XiClassData *));
    ctx->shared_enum = (const XiEnumData **) xr_calloc(CG_INIT_SHARED, sizeof(const XiEnumData *));
    ctx->shared_native_instances =
        (CgSharedNativeInstance *) xr_calloc(CG_INIT_SHARED, sizeof(CgSharedNativeInstance));
    ctx->shared_cap = CG_INIT_SHARED;
    ctx->shared_native_exports =
        (CgSharedNativeExport *) xr_calloc(CG_INIT_SHARED, sizeof(CgSharedNativeExport));
    ctx->shared_native_exports_cap = CG_INIT_SHARED;
    ctx->methods = (CgMethodEntry *) xr_calloc(CG_INIT_METHODS, sizeof(CgMethodEntry));
    ctx->methods_cap = CG_INIT_METHODS;
    ctx->imports = (CgImportEntry *) xr_calloc(CG_INIT_IMPORTS, sizeof(CgImportEntry));
    ctx->imports_cap = CG_INIT_IMPORTS;
    if (!ctx->shared_funcs || !ctx->shared_class || !ctx->shared_enum ||
        !ctx->shared_native_instances || !ctx->shared_native_exports || !ctx->methods ||
        !ctx->imports) {
        xi_cgen_ctx_free(ctx);
        return NULL;
    }
    return ctx;
}

XR_FUNC void xi_cgen_ctx_free(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->nstrlit; i++) {
        xr_free(ctx->strlit_list[i]->str);
        xr_free(ctx->strlit_list[i]);
    }
    xr_free(ctx->strlit_list);
    xr_free(ctx->shared_funcs);
    xr_free(ctx->shared_class);
    xr_free(ctx->shared_enum);
    xr_free(ctx->shared_native_instances);
    xr_free(ctx->shared_native_exports);
    xr_free(ctx->methods);
    xr_free(ctx->imports);
    xr_free(ctx->xmod_ref_funcs);
    xr_free(ctx->xmod_ref_prefixes);
    for (int i = 0; i < ctx->nemitted_funcs; i++)
        xr_free(ctx->emitted_func_names ? ctx->emitted_func_names[i] : NULL);
    xr_free(ctx->emitted_funcs);
    xr_free(ctx->emitted_func_names);
    xr_free(ctx->cell_vars);
    xr_free(ctx->cell_release_vars);
    xr_free(ctx->cell_heap_capture_vars);
    xr_free(ctx->cell_origins);
    xr_free(ctx->phi_repr);
    xr_free(ctx->array_data_cache_decls);
    xr_free(ctx->func_reach_memo);
    xr_free(ctx->shared_slot_reach_memo);
    xr_free(ctx->used_extern_decls);
    xr_free(ctx->extern_decl_adapters);
    for (size_t i = 0; i < ctx->nfunc_residues; i++)
        xr_free(ctx->func_residues[i].entries);
    xr_free(ctx->func_residues);
    xr_free(ctx);
}

XR_FUNC void xi_cgen_ctx_set_aot_bundle(XiCgenCtx *ctx, const XaotBundle *bundle) {
    if (ctx)
        ctx->aot_bundle = bundle;
}

XR_FUNC void xi_cgen_ctx_set_target(XiCgenCtx *ctx, const XaotTarget *target, bool simd_active) {
    if (ctx) {
        ctx->target = target;
        ctx->simd_active = simd_active;
    }
}

XR_FUNC void xi_cgen_ctx_set_emit_main(XiCgenCtx *ctx, bool emit_main) {
    if (ctx)
        ctx->emit_main = emit_main;
}

XR_FUNC void xi_cgen_ctx_set_freestanding_profile(XiCgenCtx *ctx, bool freestanding) {
    if (ctx)
        ctx->freestanding_profile = freestanding;
}

XR_FUNC void xi_cgen_ctx_set_type_name_profile(XiCgenCtx *ctx, XiCgenTypeNameProfile profile) {
    if (ctx)
        ctx->type_name_profile = profile;
}

XR_FUNC void xi_cgen_ctx_set_residue_tracking(XiCgenCtx *ctx, bool enabled) {
    if (ctx)
        ctx->want_residue = enabled;
}

XR_FUNC bool xi_cgen_has_error(const XiCgenCtx *ctx) {
    return ctx && ctx->error;
}

XR_FUNC XiCgenCoroFrameStats xi_cgen_coro_frame_stats(const XiCgenCtx *ctx) {
    XiCgenCoroFrameStats stats = {0};
    if (ctx)
        stats = ctx->coro_frame_stats;
    return stats;
}

XR_FUNC XiCgenStats xi_cgen_stats(const XiCgenCtx *ctx) {
    XiCgenStats stats = {0};
    if (ctx)
        stats = ctx->stats;
    return stats;
}

XR_FUNC const XiFuncResidue *xi_cgen_func_residues(const XiCgenCtx *ctx, size_t *count) {
    if (count)
        *count = ctx ? ctx->nfunc_residues : 0;
    return ctx ? ctx->func_residues : NULL;
}

XR_FUNC const char *xi_residue_category_short(XiResidueCategory category) {
    switch (category) {
        case XI_RESIDUE_R1_RUNTIME_CALL:
            return "R1";
        case XI_RESIDUE_R2_HEAP_ALLOC:
            return "R2";
        case XI_RESIDUE_R3_PENDING_ERROR:
            return "R3";
        case XI_RESIDUE_R4_BOUNDS_PANIC:
            return "R4";
        case XI_RESIDUE_R5_BOX_UNBOX:
            return "R5";
        case XI_RESIDUE_R6_LANES_ROUNDTRIP:
            return "R6";
        default:
            return "R?";
    }
}

XR_FUNC const char *xi_residue_category_label(XiResidueCategory category) {
    switch (category) {
        case XI_RESIDUE_R1_RUNTIME_CALL:
            return "runtime-helper call";
        case XI_RESIDUE_R2_HEAP_ALLOC:
            return "heap allocation";
        case XI_RESIDUE_R3_PENDING_ERROR:
            return "pending-error check";
        case XI_RESIDUE_R4_BOUNDS_PANIC:
            return "bounds-panic branch";
        case XI_RESIDUE_R5_BOX_UNBOX:
            return "XrValue box/unbox";
        case XI_RESIDUE_R6_LANES_ROUNDTRIP:
            return "aggregate<->native round-trip";
        default:
            return "unknown residue";
    }
}

XR_FUNC char *xi_cgen_residue_dump(const XiCgenCtx *ctx) {
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *out = xr_open_memstream(&buf, &bufsz);
    if (!out)
        return NULL;
    fprintf(out, "# task 217 per-function residue (R1 runtime-call, R2 heap-alloc, "
                 "R3 pending-error, R4 bounds-panic, R5 box/unbox, R6 lanes-roundtrip)\n");
    fprintf(out, "function\tsource\tR1\tR2\tR3\tR4\tR5\tR6\ttotal\n");
    size_t n = ctx ? ctx->nfunc_residues : 0;
    for (size_t i = 0; i < n; i++) {
        const XiFuncResidue *r = &ctx->func_residues[i];
        uint32_t total = 0;
        for (int c = 0; c < XI_RESIDUE_CATEGORY_COUNT; c++)
            total += r->counts[c];
        fprintf(out, "%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", r->func_name ? r->func_name : "?",
                r->source_file ? r->source_file : "-", r->counts[XI_RESIDUE_R1_RUNTIME_CALL],
                r->counts[XI_RESIDUE_R2_HEAP_ALLOC], r->counts[XI_RESIDUE_R3_PENDING_ERROR],
                r->counts[XI_RESIDUE_R4_BOUNDS_PANIC], r->counts[XI_RESIDUE_R5_BOX_UNBOX],
                r->counts[XI_RESIDUE_R6_LANES_ROUNDTRIP], total);
    }
    /* Per-occurrence detail as comment lines (ignored by TSV consumers). */
    for (size_t i = 0; i < n; i++) {
        const XiFuncResidue *r = &ctx->func_residues[i];
        for (uint32_t e = 0; e < r->nentries; e++) {
            const XiResidueEntry *ent = &r->entries[e];
            fprintf(out, "# %s %s:%u %s %s — %s\n", r->func_name ? r->func_name : "?",
                    r->source_file ? r->source_file : "-", ent->line,
                    xi_residue_category_short((XiResidueCategory) ent->category),
                    xi_residue_category_label((XiResidueCategory) ent->category),
                    ent->reason ? ent->reason : "");
        }
    }
    if (xr_close_memstream(out, &buf, &bufsz) != 0) {
        xr_free(buf);
        return NULL;
    }
    return buf;
}
