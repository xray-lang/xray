/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array_region.inc.c - Call-free scalar loops over actual Frame owners
 */
static bool c_array_region_present(const LirC *c) {
    return c->array_region && c->array_region->bound_count != 0;
}
static bool c_array_push_present(const LirC *c) {
    return c->array_region && c->array_region->has_push_window;
}
static bool c_array_region_usable(const LirC *c, size_t region) {
    const XrLirArrayRegionBinding *row=&c->array_region->bindings->regions[region];
    return row->slot_count && (row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_SET ||
        (row->region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET && c_array_helper_enabled(c,region)));
}
static void c_array_regions_destroy(LirC *c) {
    if (!c->array_regions) return;
    for (uint32_t f = 0; f < xr_lir_function_count(c->request->module); ++f) {
        xr_lir_array_region_bindings_destroy(c->array_regions[f].bindings);
        xr_lir_array_region_bindings_destroy(c->array_regions[f].push_windows);
        xr_compile_resources_free(c->array_regions[f].fixed_edges);
        xr_compile_resources_free(c->array_regions[f].helpers);
        xr_compile_resources_free(c->array_regions[f].scalars);
    }
    xr_compile_resources_free(c->array_regions);
}
static void c_array_regions_admit(LirC *c) {
    bool needed = false;
    for (uint32_t n = 0; n < c->closure_count; ++n) {
        const XrLirValueHomes *homes = c->value_homes[c->closure[n] - 1];
        needed |= homes && (homes->array_region_count || homes->array_push_window_count || homes->fixed_array_update_count);
    }
    if (!needed || !c_request_poll(c)) return;
    void *memory = NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,xr_lir_function_count(c->request->module),
        sizeof(*c->array_regions),&memory))) return;
    c->array_regions = memory;
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        uint32_t function = c->closure[n];
        const XrLirValueHomes *homes = c->value_homes[function - 1];
        if (!homes) continue;
        LirCArrayRegions *receiver = &c->array_regions[function - 1];
        if (homes->fixed_array_update_count) c_fixed_array_prepare(c, function, receiver);
        if (c->status != XR_LIR_C_OK) return;
        if (homes->array_push_window_count) {
            if (!c_value_layout_status(c,xr_lir_array_push_window_bindings_create(homes,c->value_layouts,
                &receiver->push_windows))) return;
            for (size_t r = 0; r < receiver->push_windows->region_count; ++r)
                receiver->has_push_window |= receiver->push_windows->regions[r].slot_count != 0;
            c->array_local_runtime |= receiver->has_push_window;
        }
        if (!homes->array_region_count) continue;
        if (!c_value_layout_status(c,xr_lir_array_region_bindings_create(homes,c->value_layouts,&receiver->bindings))) return;
        for (size_t r = 0; r < receiver->bindings->region_count; ++r)
            receiver->bound_count += receiver->bindings->regions[r].slot_count != 0;
        if (!receiver->bound_count) continue;
        c->array_local_runtime = true;
        c_array_helpers_prepare(c,function,receiver);
    }
}
static const XrLirArrayRegionOperation *c_array_region_operation(const LirC *c) {
    if (!c_array_region_present(c)) return NULL;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    if (c->block >= binding->block_index_count) return NULL;
    const XrLirArrayRegionBlock *block = &binding->block_index[c->block];
    if (!block->region || block->region > binding->region_count ||
        !binding->regions[block->region - 1].slot_count || !c->instruction || c->instruction > block->instruction_count)
        return NULL;
    return &binding->operations[block->first_operation + c->instruction - 1];
}
static void c_array_region_clear(LirC *c, size_t region, unsigned phase) {
    c_format(c,"    xrl_ar_%zu_cache = (XrtArrayI64RegionCache){0}; xrl_ar_%zu_phase = %u;\n",region,region,phase);
}
static void c_array_region_spec(LirC *c, const XrLirArrayRegionBindings *binding, size_t r, const char *prefix) {
    XrLirArrayRegionBinding row = binding->regions[r];
    XR_DCHECK(row.canonical_slot >= row.first_slot && row.canonical_slot-row.first_slot < row.slot_count,
        "runtime canonical index is relative to this region slot span");
    XrtArrayI64RegionSpec spec = xr_lir_array_region_guard_spec(binding, &row);
    c_format(c,"static const XrtArrayI64RegionSlot %s_slots_%" PRIu32 "_%zu[] = {\n",prefix,c->function.id,r);
    for (size_t n = 0; n < spec.slot_count; ++n) {
        XrtArrayI64RegionSlot slot = spec.slots[n];
        c_format(c,"    {%zu, %zu},\n",slot.slot,slot.offset);
    }
    c_format(c,"};\nstatic const XrtArrayI64RegionSpec %s_spec_%" PRIu32 "_%zu = {%s_slots_%" PRIu32
        "_%zu, %zu, %zu};\n",prefix,c->function.id,r,prefix,c->function.id,r,
        spec.slot_count,spec.canonical);
}
static void c_array_region_specs(LirC *c) {
    if (c_array_region_present(c)) {
        const XrLirArrayRegionBindings *binding = c->array_region->bindings;
        for (size_t r = 0; r < binding->region_count && c_request_poll(c); ++r)
            if (c_array_region_usable(c,r)) c_array_region_spec(c,binding,r,"xrl_ar");
    }
    if (c_array_push_present(c)) {
        const XrLirArrayRegionBindings *binding = c->array_region->push_windows;
        for (size_t r = 0; r < binding->region_count && c_request_poll(c); ++r)
            if (binding->regions[r].slot_count) c_array_region_spec(c,binding,r,"xrl_ap");
    }
}
static void c_array_region_declarations(LirC *c) {
    if (c_array_region_present(c)) {
        const XrLirArrayRegionBindings *binding = c->array_region->bindings;
        for (size_t r = 0; r < binding->region_count; ++r)
            if (c_array_region_usable(c,r))
                c_format(c,"    XrtArrayI64RegionCache xrl_ar_%zu_cache = {0};\n    unsigned xrl_ar_%zu_phase = 0;\n",r,r);
    }
    if (c_array_push_present(c)) {
        const XrLirArrayRegionBindings *binding = c->array_region->push_windows;
        for (size_t r = 0; r < binding->region_count; ++r)
            if (binding->regions[r].slot_count) c_format(c,"    bool xrl_ap_%zu_ready = false;\n",r);
    }
}
static void c_array_push_edge_end(LirC *c, size_t edge) {
    if (!c_array_push_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->push_windows;
    if (edge >= binding->edge_count || !binding->edges[edge].region) return;
    XrLirArrayRegionEdge role = binding->edges[edge];
    if (role.kind == XR_LIR_ARRAY_REGION_EDGE_ENTRY || role.kind == XR_LIR_ARRAY_REGION_EDGE_LATCH)
        c_format(c,"    xrl_ap_%zu_ready = %s;\n",(size_t)role.region-1,
            role.kind == XR_LIR_ARRAY_REGION_EDGE_LATCH ? "true" : "false");
}
static size_t c_array_push_window_at(const LirC *c) {
    if (!c_array_push_present(c) || c->array_helper_kind) return 0;
    const XrLirArrayRegionBindings *binding = c->array_region->push_windows;
    if (c->block >= binding->block_index_count) return 0;
    size_t window = binding->block_index[c->block].region;
    if (!window) return 0;
    const XrLirArrayRegionBinding *row = &binding->regions[window - 1];
    return row->slot_count && row->region.primary_block == c->block &&
        row->region.primary_instruction == c->instruction ? window : 0;
}
/* Enter only at the original condition, with the real owner in its header
 * home. A successful segment has exactly one observable checkpoint: the
 * next ordinary header, with its scalar induction and Array length updated. */
static void c_array_push_before_branch(LirC *c) {
    if (!c_array_push_present(c) || c->array_helper_kind) return;
    const XrLirArrayRegionBindings *binding = c->array_region->push_windows;
    if (c->block >= binding->block_index_count || !binding->block_index[c->block].region) return;
    size_t r = binding->block_index[c->block].region - 1;
    const XrLirArrayRegionBinding *row = &binding->regions[r];
    if (!row->slot_count || row->region.header != c->block) return;
    XrLirArrayPushCapacity p = row->region.push_capacity;
    c_text(c,"    if (!"); c_value(c,p.condition);
    c_format(c,") xrl_ap_%zu_ready = false;\n    else if (xrl_ap_%zu_ready && ",r,r);
    c_value(c,p.iv); c_text(c," >= 0) {\n");
    c_text(c,"        XrtArrayI64RegionCache xrl_push = {0};\n");
    c_format(c,"        if (xrt_array_i64_region_try_begin(&(XrtArrayI64RegionContext){xrl_frame, xrl_payload, "
        "&xrl_frame_locations, &xrl_frame_%" PRIu32 "}, &xrl_ap_spec_%" PRIu32 "_%zu, true, &xrl_push)) {\n",
        c->function.id,c->function.id,r);
    c_text(c,"            XrtArrayBacking *xrl_array = (XrtArrayBacking *)xrl_push.owner;\n"
        "            size_t xrl_space = xrl_array->capacity - xrl_push.length;\n"
        "            uint64_t xrl_remaining = (uint64_t)(");
    c_value(c,p.end); c_text(c," - "); c_value(c,p.iv);
    c_text(c,");\n"
        "            size_t xrl_count = xrl_remaining < xrl_space ? (size_t)xrl_remaining : xrl_space;\n"
        "            if (xrl_count) {\n"
        "                for (size_t xrl_offset = 0; xrl_offset < xrl_count; ++xrl_offset) {\n"
        "                    int64_t xrl_item = ");
    c_value(c,p.iv);
    c_text(c," + (int64_t)xrl_offset;\n"
        "                    memcpy(xrl_push.elements + (xrl_push.length + xrl_offset) * sizeof(xrl_item),\n"
        "                        &xrl_item, sizeof(xrl_item));\n"
        "                }\n"
        "                xrl_array->length = xrl_push.length + xrl_count;\n"
        "                ");
    c_value(c,p.iv); c_text(c," += (int64_t)xrl_count;\n");
    c_format(c,"                goto bb%" PRIu32 ";\n            }\n        }\n    }\n",row->region.header);
}
static void c_array_region_slow(LirC *c) {
    const XrLirArrayRegionOperation *operation = c_array_region_operation(c);
    if (!operation || !operation->access) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    size_t access = operation->access - 1;
    size_t region = binding->block_index[c->block].region - 1;
    if (!c_array_region_usable(c,region)) return;
    if (c_array_helper_enabled(c,region)) return;
    if (binding->accesses[access].operation != XR_LIR_ARRAY_OP_LEN)
        c_format(c,"xrl_ar_slow_%" PRIu32 "_%" PRIu32 ": ;\n",c->block,c->instruction);
}
static bool c_array_region_coalesced_move(LirC *c, size_t region, size_t ordinal) {
    if (!c->array_helper_kind) return false;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionBinding row = binding->regions[region];
    uint32_t expected = row.region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET ?
        XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW : XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW;
    if (row.owner_home.kind != expected) return false;
    if (ordinal < row.first_move || ordinal - row.first_move >= row.move_count) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array owner home move lost its common slice"); return true;
    }
    XrLirArraySetRegionOwnerHome fact = row.region.owner_home;
    size_t step = ordinal - row.first_move + 1;
    if (step >= fact.first_active_step && step < fact.exit_step) {
        /* The common producer discharged this exact logical transition and
         * death. Its real owner remains in the canonical Frame field. */
        c_text(c,"    ;\n"); return true;
    }
    if (step != fact.exit_step) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array owner home move is outside its common window"); return true;
    }
    XrtArrayI64RegionSlot stable = binding->slots[row.owner_home.stable_slot];
    XrtArrayI64RegionSlot exit = binding->slots[row.owner_home.exit_slot];
    XR_DCHECK(stable.slot != exit.slot && stable.offset != exit.offset,"owner exit transfers between real distinct homes");
    /* Read the actual owning field, never manufacture an owner from the
     * borrowed cache. Publish the original exit before any return/observer. */
    c_format(c,"    XrtManagedHeader *xrl_ar_owner = *(XrtManagedHeader **)(xrl_ar_payload + %zu);\n"
        "    *(XrtManagedHeader **)(xrl_ar_payload + %zu) = xrl_ar_owner;\n"
        "    *(XrtManagedHeader **)(xrl_ar_payload + %zu) = NULL;\n",stable.offset,exit.offset,stable.offset);
    return true;
}
static void c_array_region_move(LirC *c, size_t region, size_t ordinal) {
    if (c_array_region_coalesced_move(c,region,ordinal)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionMove *move = &binding->moves[ordinal];
    XR_DCHECK(move->source_slot < binding->slot_count && move->target_slot < binding->slot_count,
        "common region move selects actual projected slots");
    XrtArrayI64RegionSlot source = binding->slots[move->source_slot], target = binding->slots[move->target_slot];
    /* The once-acquired window and the common schedule prove these actual
     * fields. The pair cannot allocate, call, fail or observe an intermediate
     * representation. The cache is only a borrow; the Frame owns the result. */
    const char *payload = c->array_helper_kind ? "xrl_ar_payload" : "xrl_payload";
    c_format(c,"    *(XrtManagedHeader **)((unsigned char *)%s + %zu) = xrl_ar_%zu_cache.owner;\n"
        "    *(XrtManagedHeader **)((unsigned char *)%s + %zu) = NULL;\n",payload,target.offset,region,payload,source.offset);
}
static void c_array_region_fast_edge(LirC *c, size_t region, size_t ordinal, const XrLirEdgeView *edge) {
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionMove *move = &binding->moves[ordinal];
    XrLirBlockView target = {0};
    if (xr_lir_block_view(c->request->module,(XrLirBlock){c->request->module,c->function.id,edge->target},&target) != XR_LIR_OK) {
        c_error(c,XR_LIR_C_INVALID_LIR,"Array region edge lost its actual target"); return;
    }
    c_text(c,"    {\n");
    for (uint32_t a = 0; a < edge->argument_count; ++a) if (!c->homes->values[edge->arguments[a]].type) {
        c_format(c,"    %s xrl_ar_phi_%" PRIu32 " = ",c_type(c,c_value_type(c,edge->arguments[a])),a);
        c_value(c,edge->arguments[a]); c_text(c,";\n");
    }
    c_array_region_move(c,region,ordinal);
    for (uint32_t a = 0; a < edge->argument_count; ++a) if (!c->homes->values[edge->arguments[a]].type) {
        c_text(c,"    "); c_value(c,target.parameters[a]); c_format(c," = xrl_ar_phi_%" PRIu32 ";\n",a);
    }
    if (move->step.kind == XR_LIR_ARRAY_SET_REGION_EXIT_MOVE) {
        c_array_region_clear(c,region,0);
        if (c->array_helper_kind) c_array_helper_return(c,region);
        else c_format(c,"    goto bb%" PRIu32 ";\n",edge->target);
    } else c_format(c,"    goto xrl_ar_%zu_bb%" PRIu32 ";\n",region,edge->target);
    c_text(c,"    }\n");
}
static void c_array_region_edge_begin(LirC *c, size_t edge, const XrLirEdgeView *selected) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    if (edge >= binding->edge_count || !binding->edges[edge].region) return;
    XrLirArrayRegionEdge role = binding->edges[edge]; size_t region = role.region - 1;
    const XrLirArrayRegionBinding *row = &binding->regions[region];
    if (!c_array_region_usable(c,region)) return;
    if (role.kind == XR_LIR_ARRAY_REGION_EDGE_ENTRY || role.kind == XR_LIR_ARRAY_REGION_EDGE_EXIT) {
        c_array_region_clear(c,region,0); return;
    }
    if (role.kind != XR_LIR_ARRAY_REGION_EDGE_BODY || c->block != row->region.header) return;
    c_format(c,"    if (xrl_ar_%zu_phase == 1) {\n",region);
    c_format(c,"    if (xrt_array_i64_region_try_begin(&(XrtArrayI64RegionContext){xrl_frame, xrl_payload,"
        " &xrl_frame_locations, &xrl_frame_%" PRIu32 "}, &xrl_ar_spec_%" PRIu32 "_%zu, true, &xrl_ar_%zu_cache)) {\n",
        c->function.id,c->function.id,region,region);
    if (c_array_helper_enabled(c,region)) c_array_helper_guard(c,region);
    else {
        c_format(c,"    xrl_ar_%zu_phase = 2;\n",region);
        c_array_region_fast_edge(c,region,role.move-1,selected);
    }
    c_format(c,"    }\n    xrl_ar_%zu_phase = 3;\n    }\n",region);
}
static void c_array_region_edge_end(LirC *c, size_t edge) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    if (edge >= binding->edge_count || binding->edges[edge].kind != XR_LIR_ARRAY_REGION_EDGE_LATCH ||
        !binding->edges[edge].region) return;
    size_t region = binding->edges[edge].region - 1;
    if (c_array_region_usable(c,region))
        c_format(c,"    if (xrl_ar_%zu_phase == 0) xrl_ar_%zu_phase = 1;\n",region,region);
}
static void c_array_region_access(LirC *c, size_t region, size_t ordinal) {
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionAccess *access = &binding->accesses[ordinal];
    if (access->operation == XR_LIR_ARRAY_OP_LEN) {
        c_format(c,"    v%" PRIu32 " = (int64_t)xrl_ar_%zu_cache.length;\n",access->result,region); return;
    }
    if (!c->array_helper_kind) {
        c_text(c,"    if ("); c_value(c,access->index);
        c_text(c," < INT64_C(0) || (uint64_t)"); c_value(c,access->index);
        c_format(c," >= xrl_ar_%zu_cache.length) {\n",region);
        c_array_region_clear(c,region,3);
        c_format(c,"    goto xrl_ar_slow_%" PRIu32 "_%" PRIu32 ";\n    }\n",access->block,access->instruction);
    }
    bool fixed_copy = c->array_helper_kind == XR_LIR_ARRAY_REGION_PRIMARY_SET;
    const char *copy = fixed_copy ? "XRT_ARRAY_I64_REGION_COPY" : "memcpy";
    const char *copy_end = fixed_copy ? ");\n" : ", sizeof(int64_t));\n";
    if (access->operation == XR_LIR_ARRAY_OP_GET) {
        c_format(c,"    %s(&v%" PRIu32 ", ",copy,access->result);
        if (c->array_helper_kind) c_text(c,"xrl_ar_elements");
        else c_format(c,"xrl_ar_%zu_cache.elements",region);
        c_text(c," + (size_t)");
        c_value(c,access->index); c_text(c," * sizeof(int64_t)"); c_text(c,copy_end);
    } else {
        c_format(c,"    %s(",copy);
        if (c->array_helper_kind) c_text(c,"xrl_ar_elements");
        else c_format(c,"xrl_ar_%zu_cache.elements",region);
        c_text(c," + (size_t)"); c_value(c,access->index);
        c_text(c," * sizeof(int64_t), &"); c_value(c,access->item); c_text(c,copy_end);
    }
}
static void c_array_region_clone(LirC *c, size_t region) {
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    const XrLirArrayRegionBinding *row = &binding->regions[region];
    for (size_t n = 0; n < row->block_count && c_request_poll(c); ++n) {
        uint32_t block = binding->blocks[row->first_block + n];
        const XrLirArrayRegionBlock *index = &binding->block_index[block];
        c->block = block;
        c_format(c,"xrl_ar_%zu_bb%" PRIu32 ": ;\n",region,block);
        for (size_t i = 0; i < index->instruction_count && c_request_poll(c); ++i) {
            c->instruction = (uint32_t)i + 1;
            XrLirInstructionView operation = {0};
            if (xr_lir_instruction_view(c->request->module,(XrLirBlock){c->request->module,c->function.id,block},
                (uint32_t)i,&operation) != XR_LIR_OK) {
                c_error(c,XR_LIR_C_INVALID_LIR,"Array region clone lost its original instruction"); return;
            }
            c->source = operation.source;
            XrLirArrayRegionOperation role = binding->operations[index->first_operation + i];
            if (role.access) c_array_region_access(c,region,role.access-1);
            else if (role.move) c_array_region_move(c,region,role.move-1);
            else if (role.drop) c_text(c,"    ;\n");
            else if (operation.op == XR_LIR_BR) {
                size_t edge = index->edges[0];
                if (!edge || edge > binding->edge_count || !binding->edges[edge-1].move) {
                    c_error(c,XR_LIR_C_INVALID_LIR,"Array region branch lost its common MOVE"); return;
                }
                c_array_region_fast_edge(c,region,binding->edges[edge-1].move-1,&operation.edges[0]);
            } else if (operation.op == XR_LIR_CONDBR) {
                size_t moves[2];
                for (uint32_t e = 0; e < 2; ++e) {
                    size_t edge = index->edges[e];
                    if (!edge || edge > binding->edge_count || !binding->edges[edge-1].move) {
                        c_error(c,XR_LIR_C_INVALID_LIR,"Array region header lost its common selected MOVEs"); return;
                    }
                    moves[e] = binding->edges[edge-1].move-1;
                }
                c_text(c,"    if ("); c_value(c,operation.operands[0]); c_text(c,") {\n");
                c_array_region_fast_edge(c,region,moves[0],&operation.edges[0]); c_text(c,"    } else {\n");
                c_array_region_fast_edge(c,region,moves[1],&operation.edges[1]); c_text(c,"    }\n");
            } else {
                uint32_t source = c->location_cursor;
                c_instruction(c,&operation);
                if (source != c->location_cursor) {
                    c_error(c,XR_LIR_C_INVALID_LIR,"Array region scalar changed its original source identity"); return;
                }
            }
        }
    }
}
static void c_array_region_clones(LirC *c) {
    if (!c_array_region_present(c)) return;
    const XrLirArrayRegionBindings *binding = c->array_region->bindings;
    for (size_t r = 0; r < binding->region_count && c_request_poll(c); ++r)
        if (c_array_region_usable(c,r) && !c_array_helper_enabled(c,r)) c_array_region_clone(c,r);
}
