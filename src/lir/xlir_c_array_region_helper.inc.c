/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_array_region_helper.inc.c - Closed scalar helpers over borrowed roots
 *
 * KEY CONCEPT:
 *   Owned numeric captures describe only existing region reads and results.
 *   Actual runtime admission and disjoint spans precede restricted parameters.
 */
typedef struct LirCArrayHelperBuilder {
    LirC *c;
    const XrLirFunctionData *function;
    LirCArrayRegions *receiver;
    LirCArrayRegionHelper *helper;
    uint32_t *stamps;
    size_t *indices, region;
} LirCArrayHelperBuilder;

static bool c_array_helper_enabled(const LirC *c, size_t region) {
    return c->array_region && c->array_region->helpers && c->array_region->helpers[region].count != 0;
}
static bool c_array_helper_scalar(LirCArrayHelperBuilder *b, uint32_t value, bool read, bool output) {
    if (b->c->status != XR_LIR_C_OK) return false;
    if (!value) return true;
    const XrLirValueData definition = b->function->values[value - 1];
    if (definition.type != XR_LIR_I1 && definition.type != XR_LIR_I64) return true;
    LirCArrayRegions *receiver = b->receiver;
    if (b->stamps[value] != b->region + 1) {
        if (receiver->scalar_count == receiver->scalar_capacity) {
            size_t capacity = receiver->scalar_capacity ? receiver->scalar_capacity * 2 : 32;
            if (capacity <= receiver->scalar_capacity || capacity > SIZE_MAX / sizeof(*receiver->scalars)) {
                c_error(b->c,XR_LIR_C_OVERFLOW,"Array helper numeric projection overflow"); return false;
            }
            void *memory = receiver->scalars;
            if (!c_resource(b->c,xr_compile_resources_resize(b->c->request->resources,&memory,
                capacity * sizeof(*receiver->scalars)))) return false;
            receiver->scalars = memory; receiver->scalar_capacity = capacity;
        }
        b->indices[value] = receiver->scalar_count;
        receiver->scalars[receiver->scalar_count++] = (LirCArrayRegionScalar){.value=value,.type=definition.type};
        b->stamps[value] = (uint32_t)b->region + 1;
    }
    size_t index = b->indices[value];
    const XrLirArrayRegionBindings *bindings = receiver->bindings;
    const XrLirArraySetRegion region = bindings->regions[b->region].region;
    bool outside = definition.block >= bindings->block_index_count || !definition.block ||
        bindings->block_index[definition.block].region != b->region + 1;
    /* Header values have already been evaluated before the selected true edge.
     * Other local definitions execute on the original straight-line route. */
    if (read && (outside || definition.block == region.header) && !receiver->scalars[index].input)
        receiver->scalars[index].input = ++b->helper->inputs;
    if (output && !receiver->scalars[index].output)
        receiver->scalars[index].output = ++b->helper->outputs;
    return true;
}
static bool c_array_helper_project(LirCArrayHelperBuilder *b) {
    const XrLirArrayRegionBindings *bindings = b->receiver->bindings;
    const XrLirArrayRegionBinding row = bindings->regions[b->region];
    b->helper->first = b->receiver->scalar_count;
    for (size_t n = 0; n < row.block_count && c_request_poll(b->c); ++n) {
        uint32_t block = bindings->blocks[row.first_block + n];
        const XrLirBlockData *body = &b->function->blocks[block - 1];
        for (uint32_t a = 0; a < body->parameter_count; ++a)
            if (!c_array_helper_scalar(b,body->parameters[a],false,true)) return false;
        for (uint32_t i = 0; i < body->instruction_count && c_request_poll(b->c); ++i) {
            const XrLirInstructionView *op = &body->instructions[i].view;
            if (!c_array_helper_scalar(b,op->result,false,true)) return false;
            for (uint32_t a = 0; a < op->operand_count; ++a)
                if (!c_array_helper_scalar(b,op->operands[a],true,false)) return false;
            for (uint32_t e = 0; e < op->edge_count; ++e)
                for (uint32_t a = 0; a < op->edges[e].argument_count; ++a)
                    if (!c_array_helper_scalar(b,op->edges[e].arguments[a],true,false)) return false;
        }
    }
    if (!c_request_poll(b->c)) return false;
    const XrLirBlockData *exit = &b->function->blocks[row.region.exit_block - 1];
    for (uint32_t a = 0; a < exit->parameter_count; ++a)
        if (!c_array_helper_scalar(b,exit->parameters[a],false,true)) return false;
    if (!c_request_poll(b->c)) return false;
    /* Entry is the selected true edge, not a possibly zero-trip header. Every
     * body block executes before the latch, then the header before its sole
     * exit. Hence every local result and exit parameter is defined on return.
     * Returning them all preserves direct successor uses without a new CFG or
     * whole-function liveness analysis. No uninitialized value is filled in. */
    XR_DCHECK(b->helper->inputs && b->helper->outputs,"closed unit loop has scalar input and result values");
    b->helper->count = b->receiver->scalar_count - b->helper->first;
    return true;
}
static void c_array_helpers_prepare(LirC *c, uint32_t function, LirCArrayRegions *receiver) {
    const XrLirArrayRegionBindings *bindings = receiver->bindings;
    bool needed = false;
    for (size_t r = 0; r < bindings->region_count; ++r)
        needed |= bindings->regions[r].slot_count &&
            bindings->regions[r].region.range.kind == XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN;
    if (!needed) return;
    c->function = (XrLirFunction){c->request->module,function};
    c->block = c->instruction = 0; c->source = (XrLirSource){0};
    void *memory = NULL;
    if (!c_resource(c,xr_compile_resources_calloc(c->request->resources,bindings->region_count,
        sizeof(*receiver->helpers),&memory))) return;
    receiver->helpers = memory;
    LirCArrayHelperBuilder builder = {.c=c,.function=&c->request->module->functions[function - 1],.receiver=receiver};
    if (builder.function->value_count == UINT32_MAX) {
        c_error(c,XR_LIR_C_OVERFLOW,"Array helper value index extent overflow"); return;
    }
    size_t count = (size_t)builder.function->value_count + 1;
    memory = NULL;
    if (c_resource(c,xr_compile_resources_calloc(c->request->resources,count,sizeof(*builder.stamps),&memory))) {
        builder.stamps = memory; memory = NULL;
        if (c_resource(c,xr_compile_resources_calloc(c->request->resources,count,sizeof(*builder.indices),&memory))) {
            builder.indices = memory;
            for (size_t r = 0; r < bindings->region_count && c_request_poll(c); ++r) {
                const XrLirArrayRegionBinding row = bindings->regions[r];
                if (!row.slot_count || row.region.range.kind != XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN) continue;
                builder.region = r; builder.helper = &receiver->helpers[r];
                if (!c_array_helper_project(&builder)) break;
                if (row.region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_SET)
                    receiver->has_set_helper = true;
            }
        }
    }
    xr_compile_resources_free(builder.stamps); xr_compile_resources_free(builder.indices);
}
static void c_array_helper_types(LirC *c, size_t region) {
    const LirCArrayRegionHelper helper = c->array_region->helpers[region];
    c_format(c,"typedef struct XrlArInput_%" PRIu32 "_%zu {\n",c->function.id,region);
    for (size_t n = 0; n < helper.count; ++n) {
        LirCArrayRegionScalar scalar = c->array_region->scalars[helper.first + n];
        if (scalar.input) c_format(c,"    %s s%zu;\n",c_type(c,scalar.type),scalar.input);
    }
    c_format(c,"} XrlArInput_%" PRIu32 "_%zu;\n",c->function.id,region);
    c_format(c,"typedef struct XrlArResult_%" PRIu32 "_%zu {\n",c->function.id,region);
    for (size_t n = 0; n < helper.count; ++n) {
        LirCArrayRegionScalar scalar = c->array_region->scalars[helper.first + n];
        if (scalar.output) c_format(c,"    %s s%zu;\n",c_type(c,scalar.type),scalar.output);
    }
    c_format(c,"    XrtArrayI64RegionCache cache;\n    unsigned phase;\n} XrlArResult_%" PRIu32 "_%zu;\n",c->function.id,region);
}
static void c_array_helper_return(LirC *c, size_t region) {
    const LirCArrayRegionHelper helper = c->array_region->helpers[region];
    c_format(c,"    return (XrlArResult_%" PRIu32 "_%zu){\n",c->function.id,region);
    for (size_t n = 0; n < helper.count; ++n) {
        LirCArrayRegionScalar scalar = c->array_region->scalars[helper.first + n];
        if (scalar.output) c_format(c,"        .s%zu = v%" PRIu32 ",\n",scalar.output,scalar.value);
    }
    c_format(c,"        .cache = xrl_ar_%zu_cache, .phase = xrl_ar_%zu_phase\n    };\n",region,region);
}
static void c_array_pair_operand(LirC *c, const XrLirFunctionData *f, uint32_t value) {
    const XrLirInstructionView *def = xr_lir_internal_value_definition(f,value);
    if (def && def->op == XR_LIR_CONST)
        c_format(c,"xrt_array_i64_pair_splat(UINT64_C(%" PRIu64 "))",def->bits);
    else c_format(c,"xrl_pair_v%" PRIu32,value);
}

static void c_array_pair_prefix(LirC *c, const XrLirFunctionData *f, const XrLirArraySetRegion *row, size_t region) {
    if (!row->element_read) return;
    const XrLirBlockData *body = &f->blocks[row->primary_block - 1];
    const XrLirInstructionView *get = &body->instructions[row->element_read - 1].view;
    const XrLirInstructionView *set = &body->instructions[row->primary_instruction - 1].view;
    XR_DCHECK(get->result && set->operand_count == 3, "paired prefix consumes the original GET and SET");
    /* The caller admitted a nonempty span. Leave at least the final iteration
     * to reconstruct every original live-out, then use the original exit. */
    c_format(c,"#if defined(XRT_ARRAY_I64_PAIR_AVAILABLE)\n"
        "    const size_t xrl_pair_start = (size_t)v%" PRIu32 ";\n"
        "    const size_t xrl_pair_count = (xrl_ar_%zu_cache.length - xrl_pair_start - 1) & ~(size_t)1;\n",
        row->range.iv,region);
    c_text(c,"    unsigned char *xrl_pair_next = xrl_ar_elements + xrl_pair_start * sizeof(uint64_t);\n"
        "    const unsigned char *xrl_pair_end = xrl_pair_next + xrl_pair_count * sizeof(uint64_t);\n"
        "    while (xrl_pair_next != xrl_pair_end) {\n");
    c_format(c,"        const XrtArrayI64Pair xrl_pair_v%" PRIu32 " = xrt_array_i64_pair_load(xrl_pair_next);\n"
        "        (void)xrl_pair_v%" PRIu32 ";\n",get->result,get->result);
    for (uint32_t i = row->element_read; i + 1 < row->primary_instruction && c_request_poll(c); ++i) {
        const XrLirInstructionView *op = &body->instructions[i].view;
        if (op->op == XR_LIR_CELL_NEW) { ++i; continue; }
        if (op->op == XR_LIR_CONST) continue;
        c_format(c,"        const XrtArrayI64Pair xrl_pair_v%" PRIu32 " = ",op->result);
        if (op->op == XR_LIR_MUL) {
            const XrLirInstructionView *left = xr_lir_internal_value_definition(f,op->operands[0]);
            bool left_const = left && left->op == XR_LIR_CONST;
            const XrLirInstructionView *factor = left_const ? left : xr_lir_internal_value_definition(f,op->operands[1]);
            XR_DCHECK(factor && factor->op == XR_LIR_CONST, "paired multiplication has a proved constant factor");
            c_text(c,"xrt_array_i64_pair_scale(");
            c_array_pair_operand(c,f,op->operands[left_const]);
            c_format(c,", UINT64_C(%" PRIu64 "))",factor->bits);
        } else {
            if (op->op == XR_LIR_NEG) c_text(c,"-");
            c_array_pair_operand(c,f,op->operands[0]);
            if (op->op != XR_LIR_NEG) {
                XR_DCHECK(op->op == XR_LIR_ADD || op->op == XR_LIR_SUB,
                    "paired expression contains only proved wrapping operations");
                c_text(c,op->op == XR_LIR_ADD ? " + " : " - ");
                c_array_pair_operand(c,f,op->operands[1]);
            }
        }
        c_format(c,";\n        (void)xrl_pair_v%" PRIu32 ";\n",op->result);
    }
    c_text(c,"        xrt_array_i64_pair_store(xrl_pair_next, ");
    c_array_pair_operand(c,f,set->operands[2]);
    c_format(c,");\n        xrl_pair_next += 2 * sizeof(uint64_t);\n    }\n"
        "    v%" PRIu32 " = (int64_t)(xrl_pair_start + xrl_pair_count);\n#endif\n",row->range.iv);
}

static void c_array_helper_emit(LirC *c, size_t region) {
    const LirCArrayRegionHelper helper = c->array_region->helpers[region];
    const XrLirArrayRegionBindings *bindings = c->array_region->bindings;
    const XrLirArraySetRegion row = bindings->regions[region].region;
    c_array_helper_types(c,region);
    /* A sibling read-only window runs its own loop. Keep its code outside
     * the function containing a mutation window to limit coupled layout. */
    const char *outline = c->array_region->has_set_helper && row.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_GET ?
        "XRT_ARRAY_REGION_OUTLINE " : "";
    c_format(c,"static %sXrlArResult_%" PRIu32 "_%zu xrl_ar_loop_%" PRIu32 "_%zu(\n",
        outline,c->function.id,region,c->function.id,region);
    c_text(c,"    unsigned char *restrict xrl_ar_payload,\n    unsigned char *restrict xrl_ar_elements,\n");
    c_format(c,"    XrtArrayI64RegionCache xrl_ar_%zu_cache, unsigned xrl_ar_%zu_phase,\n"
        "    XrlArInput_%" PRIu32 "_%zu input) {\n",region,region,c->function.id,region);
    for (size_t n = 0; n < helper.count && c_request_poll(c); ++n) {
        LirCArrayRegionScalar scalar = c->array_region->scalars[helper.first + n];
        c_format(c,"    %s v%" PRIu32,c_type(c,scalar.type),scalar.value);
        if (scalar.input) c_format(c," = input.s%zu",scalar.input);
        c_format(c,";\n    (void)&v%" PRIu32 ";\n",scalar.value);
    }
    c->scratch = c->view.value_count + 1;
    c_format(c,"    uint64_t v%" PRIu32 ";\n    (void)&v%" PRIu32 ";\n",c->scratch,c->scratch);
    c_format(c,"    xrl_ar_%zu_phase = 2;\n",region);
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    if (bindings->regions[region].owner_home.kind == XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW)
        c_array_pair_prefix(c,function,&row,region);
    const XrLirBlockData *header = &function->blocks[row.header - 1];
    const XrLirInstructionView *term = &header->instructions[header->instruction_count - 1].view;
    size_t edge = bindings->block_index[row.header].edges[0];
    XR_DCHECK(edge && bindings->edges[edge - 1].move,"common range starts on the selected true body edge");
    c->array_helper_kind = row.primary_kind;
    c_array_region_fast_edge(c,region,bindings->edges[edge - 1].move - 1,&term->edges[0]);
    c_array_region_clone(c,region);
    c->array_helper_kind = XR_LIR_ARRAY_REGION_PRIMARY_NONE;
    c_text(c,"}\n\n");
}
static void c_array_helpers(LirC *c) {
    for (uint32_t n = 0; n < c->closure_count && c_request_poll(c); ++n) {
        c->function = (XrLirFunction){c->request->module,c->closure[n]};
        xr_lir_function_view(c->request->module,c->function,&c->view);
        c_values_function(c);
        if (!c_array_region_present(c)) continue;
        for (size_t r = 0; r < c->array_region->bindings->region_count && c_request_poll(c); ++r)
            if (c_array_helper_enabled(c,r)) c_array_helper_emit(c,r);
    }
}
static void c_array_helper_call(LirC *c, size_t region) {
    const LirCArrayRegionHelper helper = c->array_region->helpers[region];
    c_format(c,"    XrlArResult_%" PRIu32 "_%zu xrl_ar_result = xrl_ar_loop_%" PRIu32 "_%zu(\n"
        "        (unsigned char *)xrl_payload, xrl_ar_%zu_cache.elements, xrl_ar_%zu_cache, xrl_ar_%zu_phase,\n"
        "        (XrlArInput_%" PRIu32 "_%zu){\n",c->function.id,region,c->function.id,region,region,region,region,c->function.id,region);
    for (size_t n = 0; n < helper.count; ++n) {
        LirCArrayRegionScalar scalar = c->array_region->scalars[helper.first + n];
        if (scalar.input) {
            c_format(c,"            .s%zu = ",scalar.input); c_value(c,scalar.value); c_text(c,",\n");
        }
    }
    c_text(c,"        });\n");
    for (size_t n = 0; n < helper.count; ++n) {
        LirCArrayRegionScalar scalar = c->array_region->scalars[helper.first + n];
        if (scalar.output) {
            c_text(c,"    "); c_value(c,scalar.value); c_format(c," = xrl_ar_result.s%zu;\n",scalar.output);
        }
    }
    c_format(c,"    xrl_ar_%zu_cache = xrl_ar_result.cache; xrl_ar_%zu_phase = xrl_ar_result.phase;\n"
        "    goto bb%" PRIu32 ";\n",region,region,c->array_region->bindings->regions[region].region.exit_block);
}
static void c_array_helper_guard(LirC *c, size_t region) {
    /* The borrowed cache is live only after full runtime admission. Check full
     * actual spans before introducing two independently restricted roots. */
    c_text(c,"    bool xrl_ar_disjoint = false;\n");
    c_format(c,"    if (xrl_ar_%zu_cache.length <= SIZE_MAX / sizeof(int64_t)) {\n"
        "        const size_t xrl_ar_bytes = xrl_ar_%zu_cache.length * sizeof(int64_t);\n",region,region);
    c_format(c,"        if ((uintmax_t)xrl_frame_%" PRIu32 ".payload_size <= (uintmax_t)UINTPTR_MAX &&\n"
        "            (uintmax_t)xrl_ar_bytes <= (uintmax_t)UINTPTR_MAX) {\n",c->function.id);
    c_text(c,"            const uintptr_t xrl_ar_payload_begin = (uintptr_t)xrl_payload;\n");
    c_format(c,"            const uintptr_t xrl_ar_elements_begin = (uintptr_t)xrl_ar_%zu_cache.elements;\n"
        "            const uintptr_t xrl_ar_payload_bytes = (uintptr_t)xrl_frame_%" PRIu32 ".payload_size;\n",region,c->function.id);
    c_text(c,"            const uintptr_t xrl_ar_element_bytes = (uintptr_t)xrl_ar_bytes;\n"
        "            if (xrl_ar_payload_bytes <= UINTPTR_MAX - xrl_ar_payload_begin &&\n"
        "                xrl_ar_element_bytes <= UINTPTR_MAX - xrl_ar_elements_begin)\n"
        "                xrl_ar_disjoint = !xrl_ar_payload_bytes || !xrl_ar_element_bytes ||\n"
        "                    xrl_ar_payload_begin + xrl_ar_payload_bytes <= xrl_ar_elements_begin ||\n"
        "                    xrl_ar_elements_begin + xrl_ar_element_bytes <= xrl_ar_payload_begin;\n"
        "        }\n    }\n    if (xrl_ar_disjoint && ");
    uint32_t iv = c->array_region->bindings->regions[region].region.range.iv;
    c_value(c,iv); c_text(c," >= INT64_C(0) &&\n");
    c_format(c,"        (uintmax_t)xrl_ar_%zu_cache.length <= (uintmax_t)INT64_MAX &&\n        (uintmax_t)(uint64_t)",region);
    c_value(c,iv); c_format(c," < (uintmax_t)xrl_ar_%zu_cache.length) {\n",region);
    c_array_helper_call(c,region);
    c_text(c,"    }\n");
    c_format(c,"    xrl_ar_%zu_cache = (XrtArrayI64RegionCache){0};\n",region);
}
