/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_fixed_array_update.inc.c - Checked projection of a fixed Array update loop
 *
 * KEY CONCEPT:
 *   The entry guard may borrow a private owner immediately.
 *   A shared or otherwise unavailable owner still takes the ordinary path.
 */

static LirCFixedArrayEdge *c_fixed_array_edge(LirCFixedArrayEdge *index, uint32_t block, uint32_t edge) {
    return &index[(size_t)block * 2 + edge - 1];
}

static void c_fixed_array_prepare(LirC *c, uint32_t function, LirCArrayRegions *receiver) {
    const XrLirValueHomes *homes = c->value_homes[function - 1];
    XrLirFunctionView view;
    if (xr_lir_function_view(c->request->module, (XrLirFunction){c->request->module, function}, &view) != XR_LIR_OK) {
        c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array update lost its function");
        return;
    }
    if ((uint64_t)view.block_count + 1 > SIZE_MAX / 2 / sizeof(*receiver->fixed_edges)) {
        c_error(c, XR_LIR_C_OVERFLOW, "fixed Array edge index exceeds address space");
        return;
    }
    void *memory = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
        ((size_t)view.block_count + 1) * 2, sizeof(*receiver->fixed_edges), &memory))) return;
    receiver->fixed_edges = memory;
    c->array_local_runtime = true;
    for (uint32_t n = 0; n < homes->fixed_array_update_count && c_request_poll(c); ++n) {
        const XrLirFixedArrayUpdateHome *row = &homes->fixed_array_updates[n];
        const XrLirArraySetRegion *r = &row->region;
        if (!r->entry_block || r->entry_block > view.block_count || !r->entry_edge || r->entry_edge > 2 ||
            !r->header || r->header > view.block_count || !r->latch || r->latch > view.block_count ||
            !row->body_edge || row->body_edge > 2 || !r->exit_edge || r->exit_edge > 2 ||
            r->first_step > homes->fixed_array_update_step_count || r->step_count < 3 ||
            r->step_count > homes->fixed_array_update_step_count - r->first_step) {
            c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array update lost its selected edges");
            return;
        }
        LirCFixedArrayEdge *entry = c_fixed_array_edge(receiver->fixed_edges, r->entry_block, r->entry_edge);
        LirCFixedArrayEdge *latch = c_fixed_array_edge(receiver->fixed_edges, r->latch, 1);
        LirCFixedArrayEdge *exit = c_fixed_array_edge(receiver->fixed_edges, r->header, r->exit_edge);
        if (entry->entry || latch->latch || exit->exit) {
            c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array updates overlap a selected edge");
            return;
        }
        entry->entry = latch->latch = n + 1;
        if (r->primary_kind != XR_LIR_ARRAY_REGION_PRIMARY_PUSH) exit->exit = n + 1;
    }
}

static void c_fixed_array_declarations(LirC *c) {
    if (!c->array_region || !c->array_region->fixed_edges) return;
    for (uint32_t n = 0; n < c->homes->fixed_array_update_count && c_request_poll(c); ++n) {
        c_format(c, "    _Bool xrl_fixed_tried_%" PRIu32 " = 0;\n"
            "    XrtArrayI64RegionCache xrl_fixed_cache_%" PRIu32 " = {0};\n", n + 1, n + 1);
    }
}

static void c_fixed_array_limit(LirC *c, const XrLirArraySetRegion *region, uint32_t number) {
    if (region->range.kind == XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN) {
        c_format(c, "(int64_t)xrl_fixed_cache_%" PRIu32 ".length", number);
        return;
    }
    /* A constant defined at the header has not executed at the entry edge. */
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    const XrLirInstructionView *value = xr_lir_internal_value_definition(function, region->range.length);
    if (!value || value->op != XR_LIR_CONST)
        c_value(c, region->range.length);
    else if (value->bits <= INT64_MAX)
        c_format(c, "INT64_C(%" PRIu64 ")", value->bits);
    else
        c_format(c, "(-INT64_C(1) - INT64_C(%" PRIu64 "))", ~value->bits);
}

static void c_fixed_array_guard(LirC *c, uint32_t n, uint32_t target, bool after_write) {
    const XrLirFixedArrayUpdateHome *row = &c->homes->fixed_array_updates[n - 1];
    XrtArrayI64RegionSlot slots[XR_LIR_FIXED_ARRAY_GUARD_SLOT_COUNT];
    XrtArrayI64RegionSpec spec = xr_lir_fixed_array_guard_spec(c->homes, row, slots);
    bool push = row->region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_PUSH;
    if (after_write) c_format(c, "    if (!xrl_fixed_tried_%" PRIu32 ") {\n", n);
    else c_text(c, "    {\n");
    c_text(c, "        static const XrtArrayI64RegionSlot xrl_fixed_slots[] = {");
    for (size_t slot = 0; slot < spec.slot_count; ++slot) {
        if (slot) c_text(c, ", ");
        c_format(c, "{%zu, %zu}", slots[slot].slot, slots[slot].offset);
    }
    c_text(c, "};\n");
    c_format(c, "        const XrtArrayI64RegionSpec xrl_fixed_spec = {xrl_fixed_slots, %zu, %zu};\n"
        "        const XrtArrayI64RegionContext xrl_fixed_context = {xrl_frame, xrl_payload,"
        " &xrl_frame_locations, &xrl_frame_%" PRIu32 "};\n        if (", spec.slot_count, spec.canonical, c->function.id);
    c_value(c, row->region.range.iv);
    c_format(c, " >= INT64_C(0) && xrt_array_i64_region_try_begin(&xrl_fixed_context, &xrl_fixed_spec, true,"
        " &xrl_fixed_cache_%" PRIu32 ")", n);
    if (row->region.range.kind == XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT) {
        if (push) {
            c_format(c, " && xrl_fixed_cache_%" PRIu32 ".length < "
                "((const XrtArrayBacking *)xrl_fixed_cache_%" PRIu32 ".owner)->capacity && ", n, n);
        } else {
            c_text(c, " && (uint64_t)(");
            c_fixed_array_limit(c, &row->region, n);
            c_format(c, ") <= xrl_fixed_cache_%" PRIu32 ".length && ", n);
        }
        c_value(c, row->region.range.iv);
        c_text(c, " < ");
        c_fixed_array_limit(c, &row->region, n);
        c_text(c, ") {\n");
    } else {
        c_text(c, " && (uint64_t)");
        c_value(c, row->region.range.iv);
        c_format(c, " < xrl_fixed_cache_%" PRIu32 ".length) {\n", n);
    }
    c_array_held_clear(c);
    c_format(c, "            goto xrl_fixed_%" PRIu32 "_bb%" PRIu32 ";\n"
        "        }\n"
        "        xrl_fixed_cache_%" PRIu32 " = (XrtArrayI64RegionCache){0};\n", n, target, n);
    /* Each successful ordinary push can establish fresh spare capacity. */
    if (after_write && !push) c_format(c, "        xrl_fixed_tried_%" PRIu32 " = 1;\n", n);
    c_text(c, "    }\n");
}

static void c_fixed_array_access(LirC *c, uint32_t n, const XrLirInstructionView *v) {
    XrLirArrayIntrinsic shape = {0};
    if (!c_array_intrinsic(c, v, &shape)) {
        c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array update lost its original operation");
        return;
    }
    if (shape.operation == XR_LIR_ARRAY_OP_LEN) {
        c_format(c, "    v%" PRIu32 " = (int64_t)xrl_fixed_cache_%" PRIu32 ".length;\n", v->result, n);
        return;
    }
    if (shape.operation == XR_LIR_ARRAY_OP_PUSH) {
        c_format(c, "    XRT_ARRAY_I64_REGION_COPY(xrl_fixed_cache_%" PRIu32 ".elements + "
            "xrl_fixed_cache_%" PRIu32 ".length * sizeof(int64_t), &", n, n);
        c_value(c, v->operands[1]);
        c_format(c, ");\n    ++xrl_fixed_cache_%" PRIu32 ".length;\n", n);
        return;
    }
    if (shape.operation != XR_LIR_ARRAY_OP_GET && shape.operation != XR_LIR_ARRAY_OP_SET) {
        c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array update cannot change its backing size");
        return;
    }
    bool read = shape.operation == XR_LIR_ARRAY_OP_GET;
    c_text(c, "    XRT_ARRAY_I64_REGION_COPY(");
    if (read) c_format(c, "&v%" PRIu32 ", ", v->result);
    c_format(c, "xrl_fixed_cache_%" PRIu32 ".elements + (size_t)", n);
    c_value(c, v->operands[1]);
    c_text(c, " * sizeof(int64_t)");
    if (!read) {
        c_text(c, ", &");
        c_value(c, v->operands[2]);
    }
    c_text(c, ");\n");
}

static const XrLirInstructionView *c_fixed_array_block(LirC *c, uint32_t n,
    const XrLirArraySetRegionStep *first, const XrLirArraySetRegionStep *edge) {
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    const XrLirBlockData *block = &function->blocks[edge->block - 1];
    c->block = edge->block;
    for (uint32_t i = 0; i + 1 < block->instruction_count && c_request_poll(c); ++i) {
        c->instruction = i + 1;
        if (first != edge && c->instruction == first->instruction) {
            /* The common home made both halves of this Cell transfer implicit. */
            i = first->drop - 1;
            ++first;
            continue;
        }
        const XrLirInstructionView *v = &block->instructions[i].view;
        if (v->op == XR_LIR_CALL_INTRINSIC) c_fixed_array_access(c, n, v);
        else {
            uint32_t source = c->location_cursor;
            c->source = v->source;
            c_instruction(c, v);
            if (source != c->location_cursor) {
                c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array scalar changed its original source location");
                return NULL;
            }
        }
    }
    if (first != edge) {
        c_error(c, XR_LIR_C_INVALID_LIR, "fixed Array update lost a Cell transfer");
        return NULL;
    }
    return &block->instructions[block->instruction_count - 1].view;
}

static void c_fixed_array_iteration(LirC *c, uint32_t label, const XrLirArraySetRegion *region,
    const XrLirArraySetRegionStep *steps) {
    const XrLirArraySetRegionStep *first = steps + 1;
    for (uint32_t s = 1; s < region->step_count - 1 && c_request_poll(c); ++s) {
        const XrLirArraySetRegionStep *step = &steps[s];
        if (step->kind != XR_LIR_ARRAY_SET_REGION_EDGE_MOVE) continue;
        const XrLirInstructionView *term = c_fixed_array_block(c, label, first, step);
        if (!term) return;
        uint32_t skipped = step->block == region->latch ? region->range.iv : 0;
        c_scalar_edge_values(c, &term->edges[step->edge - 1], skipped);
        c_text(c, "    }\n");
        first = step + 1;
    }
}

static void c_fixed_array_push_clone(LirC *c, uint32_t label, const XrLirArraySetRegion *region,
    const XrLirArraySetRegionStep *steps) {
    uint32_t iv = region->range.iv;
    /* Entry proved a nonnegative IV below the original limit. Cap the span
     * by actual spare capacity, so neither the count nor the final IV wraps. */
    c_format(c, "    {\n"
        "        XrtArrayBacking *xrl_array = (XrtArrayBacking *)xrl_fixed_cache_%" PRIu32 ".owner;\n", label);
    c_format(c, "        size_t xrl_room = xrl_array->capacity - xrl_fixed_cache_%" PRIu32 ".length;\n", label);
    c_text(c, "        uint64_t xrl_remaining = (uint64_t)(");
    c_fixed_array_limit(c, region, label);
    c_format(c, ") - (uint64_t)v%" PRIu32 ";\n"
        "        size_t xrl_count = xrl_remaining < xrl_room ? (size_t)xrl_remaining : xrl_room;\n", iv);
    if (region->push_counted) {
        c_text(c, "        for (size_t xrl_offset = 0; xrl_offset < xrl_count; ++xrl_offset) {\n");
        c_format(c, "            int64_t xrl_item = v%" PRIu32 " + (int64_t)xrl_offset;\n", iv);
        c_format(c, "            XRT_ARRAY_I64_REGION_COPY(xrl_fixed_cache_%" PRIu32 ".elements + "
            "(xrl_fixed_cache_%" PRIu32 ".length + xrl_offset) * sizeof(xrl_item), &xrl_item);\n", label, label);
        c_format(c, "        }\n"
            "        v%" PRIu32 " += (int64_t)xrl_count;\n"
            "        xrl_fixed_cache_%" PRIu32 ".length += xrl_count;\n", iv, label);
    } else {
        c_format(c, "        int64_t xrl_stop = v%" PRIu32 " + (int64_t)xrl_count;\n"
            "        for (; v%" PRIu32 " < xrl_stop; ++v%" PRIu32 ") {\n", iv, iv, iv);
        c_fixed_array_iteration(c, label, region, steps);
        c_text(c, "        }\n");
    }
    c_format(c, "        xrl_array->length = xrl_fixed_cache_%" PRIu32 ".length;\n    }\n"
        "    xrl_fixed_cache_%" PRIu32 " = (XrtArrayI64RegionCache){0};\n"
        "    goto bb%" PRIu32 ";\n", label, label, region->header);
}

static void c_fixed_array_clones(LirC *c) {
    if (!c->array_region || !c->array_region->fixed_edges) return;
    c->fixed_array_loop = true;
    for (uint32_t n = 0; n < c->homes->fixed_array_update_count && c_request_poll(c); ++n) {
        const XrLirFixedArrayUpdateHome *row = &c->homes->fixed_array_updates[n];
        const XrLirArraySetRegion *region = &row->region;
        const XrLirArraySetRegionStep *steps = c->homes->fixed_array_update_steps + region->first_step;
        uint32_t iv = region->range.iv, label = n + 1;
        c_format(c, "xrl_fixed_%" PRIu32 "_bb%" PRIu32 ": ;\n", label, region->header);
        if (region->primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_PUSH) {
            c_fixed_array_push_clone(c, label, region, steps);
            continue;
        }
        if (region->primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_SET) {
            /* Admission bounds the IV and the unchanged length. Run the same
             * iterations and phi copies in order inside each known-size span.
             * A host compiler can then optimize the count without a tail inside
             * that span. It still decides which operations can be vectorized. */
            c_format(c, "#if defined(XRT_ARRAY_I64_FIXED_SPANS)\n    {\n"
                "        enum { xrl_fixed_span = 64 };\n"
                "        const size_t xrl_fixed_end = xrl_fixed_cache_%" PRIu32 ".length;\n",
                label);
            c_format(c,
                "        while (xrl_fixed_end - (size_t)v%" PRIu32 " >= xrl_fixed_span) {\n"
                "            const size_t xrl_fixed_start = (size_t)v%" PRIu32 ";\n", iv, iv);
            c_format(c,
                "            for (size_t xrl_fixed_offset = 0; xrl_fixed_offset < xrl_fixed_span; ++xrl_fixed_offset) {\n"
                "                v%" PRIu32 " = (int64_t)(xrl_fixed_start + xrl_fixed_offset);\n", iv);
            c_fixed_array_iteration(c, label, region, steps);
            c_format(c, "            }\n"
                "            v%" PRIu32 " = (int64_t)(xrl_fixed_start + xrl_fixed_span);\n"
                "        }\n"
                "    }\n#endif\n", iv);
        }
        c_format(c, "    for (; v%" PRIu32 " < ", iv);
        c_fixed_array_limit(c, region, label);
        c_format(c, "; ++v%" PRIu32 ") {\n", iv);
        c_fixed_array_iteration(c, label, region, steps);
        c_text(c, "    }\n");
        /* The exit may read values computed by the original final header. */
        (void)c_fixed_array_block(c, label, steps + 1, steps + 1);
        c_format(c, "    xrl_fixed_cache_%" PRIu32 " = (XrtArrayI64RegionCache){0};\n"
            "    goto xrl_fixed_exit_%" PRIu32 ";\n", label, label);
    }
    c->fixed_array_loop = false;
}
