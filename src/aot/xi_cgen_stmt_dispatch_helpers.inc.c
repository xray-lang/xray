/*
 * xi_cgen_stmt_dispatch_helpers.inc.c - Generated Xi statement lowering drivers for AOT C
 */

static uint32_t xicgen_stmt_bound_try_id(const XiFunc *f, const XiValue *v) {
    if (v && v->aux) {
        const XiValue *try_op = (const XiValue *) v->aux;
        if (try_op->op == XI_TRY)
            return try_op->id;
    }

    uint32_t try_id = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *tv = blk->values[vi];
            if (tv && tv->op == XI_TRY)
                try_id = tv->id;
        }
    }
    return try_id;
}

static bool xicgen_stmt_codegen_compiler_fence(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *v, const char *prefix) {
    (void) f;
    (void) v;
    (void) prefix;
    const char *adapter = cg_codegen_fence_adapter_name(ctx);
    XrCodegenFencePlan plan = XR_CODEGEN_FENCE_OWNER_PLAN(
        XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_HI,
        XR_SEM_OWNER_ID_SHARED_CODEGEN_COMPILER_FENCE_LO, XR_SEM_CONSUMER_CGEN);
    if (!adapter || !xr_codegen_fence_plan_is_exact_core(plan))
        return false;
    fprintf(out, "    xrt_codegen_compiler_fence();\n");
    return true;
}

static bool xicgen_stmt_try(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                            const char *prefix) {
    (void) ctx;
    (void) f;
    (void) prefix;
    const XiBlock *catch_blk = (const XiBlock *) v->aux;
    fprintf(out, "    XrtExcFrame _ef%u;\n", v->id);
    fprintf(out, "    _ef%u.prev = xrt_exc_top;\n", v->id);
    fprintf(out, "    xrt_exc_top = &_ef%u;\n", v->id);
    if (catch_blk) {
        fprintf(out,
                "    if (setjmp(_ef%u.buf) != 0) {"
                " xrt_exc_top = _ef%u.prev; goto L%u; }\n",
                v->id, v->id, catch_blk->id);
    } else {
        fprintf(out,
                "    if (setjmp(_ef%u.buf) != 0) {"
                " xrt_exc_top = _ef%u.prev; }\n",
                v->id, v->id);
    }
    return true;
}

static bool xicgen_stmt_end_try(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) ctx;
    (void) v;
    (void) prefix;
    fprintf(out, "    xrt_exc_top = _ef%u.prev;\n", xicgen_stmt_bound_try_id(f, v));
    return true;
}

static bool xicgen_stmt_catch(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) prefix;
    XrCValueEmissionView authority = {0};
    if (!cg_panic_catch_emission_authority(ctx, f, v, &authority)) {
        ctx->error = true;
        fprintf(stderr, "[xi_cgen] ERROR: panic catch lacks immutable emission authority\n");
        return false;
    }
    fprintf(out, "    ");
    if (!ctx->pre_decl_all) {
        fprintf(out, "%s ", authority.c_type);
        emit_vref(out, v);
        fprintf(out, " = ");
    } else {
        emit_vref(out, v);
        fprintf(out, " = ");
    }
    fprintf(out, "_ef%u.exception;\n", xicgen_stmt_bound_try_id(f, v));
    return true;
}

static bool xicgen_value_is_enum_aggregate_error(XiCgenCtx *ctx, const XiValue *v) {
    return ctx && cg_value_plan_is_adt_aggregate(ctx, v);
}

static bool xicgen_value_is_freestanding_enum_aggregate_error(XiCgenCtx *ctx, const XiValue *v) {
    return ctx && ctx->freestanding_profile && xicgen_value_is_enum_aggregate_error(ctx, v);
}

static void xicgen_emit_clear_freestanding_enum_error(XiCgenCtx *ctx, FILE *out) {
    if (!ctx || !ctx->freestanding_profile)
        return;
    fprintf(out, "    xrt_pending_enum_error = xrt_enum_aggregate_zero();\n");
    fprintf(out, "    xrt_pending_enum_error_active = 0;\n");
}

static void xicgen_emit_set_pending_error(XiCgenCtx *ctx, FILE *out, const XiValue *error) {
    if (xicgen_value_is_freestanding_enum_aggregate_error(ctx, error)) {
        fprintf(out, "    xrt_pending_enum_error = ");
        emit_adt_aggregate_as_base_expr(ctx, out, error);
        fprintf(out, ";\n");
        fprintf(out, "    xrt_pending_enum_error_active = 1;\n");
        fprintf(out, "    xrt_pending_error = XR_NULL_VAL;\n");
        return;
    }

    fprintf(out, "    xrt_pending_error = ");
    emit_value_as_rep_ctx(ctx, out, error, XR_REP_TAGGED);
    fprintf(out, ";\n");
    xicgen_emit_clear_freestanding_enum_error(ctx, out);
}

static bool xicgen_stmt_err_set(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                const char *prefix) {
    (void) f;
    (void) prefix;
    XR_DCHECK(v->nargs >= 1, "xicgen_stmt_err_set: missing error value");
    xicgen_emit_set_pending_error(ctx, out, v->args[0]);
    return true;
}

static bool xicgen_stmt_err_return(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                   const char *prefix) {
    XR_DCHECK(v->nargs >= 1, "xicgen_stmt_err_return: missing error value");
    xicgen_emit_set_pending_error(ctx, out, v->args[0]);
    emit_class_field_cache_flush(ctx, out);
    emit_default_return_stmt_for_abi(ctx, out, f);
    return true;
}

static bool xicgen_err_check_elided_by_func_attr(XiCgenCtx *ctx, const XiFunc *f,
                                                 const XiValue *v) {
    return v && v->op == XI_ERR_CHECK && !cg_value_type_is_bool(v) &&
           xaot_bundle_find_func_attr_plan(cg_ctx_aot_bundle(ctx), f) != NULL;
}

/* Unit ERR_CHECK has an implicit function-exit edge.  ARC records the owners
 * that remain live on the normal continuation in args[]; release them only
 * inside the already-cold pending-error branch.  A synthetic RELEASE reuses
 * the authoritative representation conversion used by ordinary ARC drops. */
static void xicgen_emit_err_check_arc_cleanups(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                               const XiValue *check, const char *prefix) {
    if (!xi_err_check_has_arc_cleanups(check))
        return;
    for (uint16_t i = XI_ERR_CHECK_CLEANUP_ARG_BASE; i < check->nargs; i++) {
        XiValue *owner = check->args[i];
        if (!owner)
            continue;
        /* await-all scalarization erases the temporary task array and its
         * pushes.  ARC cleanup metadata is deliberately attached after the
         * optimization pipeline, so omit a cold-edge drop for storage that
         * CGen proves has no materialized representation. */
        if (cg_await_all_inline_literal_value_is_elided(f, owner))
            continue;
        XiValue *drop_args[1] = {owner};
        XiValue drop = {
            .op = XI_RELEASE,
            .type = owner->type,
            .args = drop_args,
            .nargs = 1,
        };
        fprintf(out, "        ");
        xicgen_release(ctx, out, f, &drop, prefix);
        fprintf(out, ";\n");
    }
}

static bool xicgen_stmt_err_check(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    if (cg_value_type_is_bool(v)) {
        XrRep rep = cg_value_plan_storage_rep(ctx, v);
        fprintf(out, "    ");
        if (!ctx->pre_decl_all) {
            fprintf(out, "%s ", ctype_str(rep));
            emit_vref(out, v);
            fprintf(out, " = ");
        } else {
            emit_vref(out, v);
            fprintf(out, " = ");
        }
        if (rep == XR_REP_TAGGED)
            fprintf(out, "XR_FROM_BOOL(xrt_has_pending_error());\n");
        else
            fprintf(out, "xrt_has_pending_error();\n");
        return true;
    }

    if (xicgen_err_check_after_proven_nothrow(ctx, f, v) ||
        xicgen_err_check_elided_by_func_attr(ctx, f, v) ||
        cg_array_err_check_after_unchecked_fill_push(ctx, f, v) ||
        cg_array_err_check_after_direct_byte_array_mutator_trusted(ctx, f, v) ||
        cg_array_err_check_after_index_get_trusted(ctx, f, v) ||
        cg_array_err_check_after_byte_array_append_trusted(ctx, f, v) ||
        cg_array_err_check_after_typed_push(ctx, f, v) ||
        cg_class_native_err_check_after_nothrow_call(ctx, f, v))
        return true;

    fprintf(out, "    if (XR_UNLIKELY(xrt_has_pending_error())) {\n");
    emit_class_field_cache_flush(ctx, out);
    xicgen_emit_err_check_arc_cleanups(ctx, out, f, v, prefix);
    if (cg_func_return_abi_rep(ctx, f) == XR_REP_VOID) {
        fprintf(out, "        return;\n");
    } else {
        fprintf(out, "        return ");
        emit_default_return_for_abi(ctx, out, f);
        fprintf(out, ";\n");
    }
    fprintf(out, "    }\n");
    return true;
}

static bool xicgen_stmt_err_catch(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                  const char *prefix) {
    (void) prefix;
    const XaotValuePlan *plan = cg_value_plan_require_legacy(ctx, v);
    bool aggregate_error = xicgen_value_is_enum_aggregate_error(ctx, v);
    bool freestanding_aggregate = ctx->freestanding_profile && aggregate_error;
    /* The only site left reading XiValue.rep. It is not unmigrated: an enum
     * aggregate error is a stated exception. Measured over the whole corpus,
     * this branch is never taken, so whether the plan would answer the same
     * cannot be decided here -- which is why it stays rather than being
     * switched on the assumption that it would. */
    XrRep catch_rep = aggregate_error ? cg_rep(v) : cg_value_plan_storage_rep(ctx, v);
    fprintf(out, "    ");
    if (!ctx->pre_decl_all) {
        fprintf(out, "%s ",
                aggregate_error ? local_ctype_str_ctx(ctx, f, v) : ctype_str(catch_rep));
        emit_vref(out, v);
        fprintf(out, " = ");
    } else {
        emit_vref(out, v);
        fprintf(out, " = ");
    }
    if (aggregate_error) {
        if (plan && cg_value_rep_is_typed_adt_aggregate(plan->rep))
            fprintf(out, "%s_from_base(", plan->rep.c_type);
        if (freestanding_aggregate) {
            fprintf(out, "xrt_pending_enum_error");
        } else {
            fprintf(out, "xrt_enum_aggregate_take_from_boxed(xrt_pending_error)");
        }
        if (plan && cg_value_rep_is_typed_adt_aggregate(plan->rep))
            fprintf(out, ")");
        fprintf(out, ";\n");
    } else {
        const char *suffix = emit_conversion_prefix(out, v->type, XR_REP_TAGGED, catch_rep);
        fprintf(out, "xrt_pending_error");
        emit_conversion_suffix(out, suffix);
        fprintf(out, ";\n");
    }
    fprintf(out, "    xrt_pending_error = XR_NULL_VAL;\n");
    xicgen_emit_clear_freestanding_enum_error(ctx, out);
    return true;
}

static bool xicgen_stmt_cleanup_enter(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix) {
    (void) ctx;
    (void) f;
    (void) v;
    (void) prefix;
    fprintf(out, "    xrt_cleanup_enter();\n");
    return true;
}

static bool xicgen_stmt_cleanup_leave(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                                      const char *prefix) {
    (void) ctx;
    (void) f;
    (void) v;
    (void) prefix;
    fprintf(out, "    xrt_cleanup_leave();\n");
    return true;
}

static bool xicgen_stmt_cleanup_err_check(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                          const XiValue *v, const char *prefix) {
    (void) ctx;
    (void) f;
    (void) v;
    (void) prefix;
    fprintf(out, "    xrt_cleanup_err_check();\n");
    return true;
}

static bool xi_to_c_emit_stmt_generated(XiCgenCtx *ctx, FILE *out, const XiFunc *f,
                                        const XiValue *v, const char *prefix) {
    switch (v->op) {
#define XICGEN_STMT_GENERATED_CASE(op, name, driver)                                               \
    case XI_##op:                                                                                  \
        (void) name;                                                                               \
        return driver(ctx, out, f, v, prefix);
        XI_TO_C_STMT_LOWERING_DRIVERS(XICGEN_STMT_GENERATED_CASE)
#undef XICGEN_STMT_GENERATED_CASE
        case XI_OP_COUNT:
            break;
    }
    return false;
}
