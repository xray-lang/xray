/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c_value_transfers.inc.c - Ordinary C around shared stable ownership homes
 *
 * KEY CONCEPT:
 *   Copied final ownership events select copies or moves. Common homes supply real
 *   owner and temporary positions; publication changes only non-owner bytes.
 */
typedef struct LirCEdgeEvents {
    const XrLirValueHomes *homes;
    size_t *index, capacity;
} LirCEdgeEvents;
static size_t c_edge_event_hash(uint32_t block, uint32_t edge, uint32_t target) {
    uint64_t key = ((uint64_t)block << 32) | edge;
    key ^= (uint64_t)target * UINT64_C(0x9e3779b97f4a7c15);
    return (size_t)(key ^ (key >> 29));
}
static void c_edge_events_prepare(LirC *c, const XrLirValueHomes *homes, LirCEdgeEvents *events) {
    if (!c_request_poll(c)) return;
    events->homes = homes;
    size_t count = 0;
    for (size_t i = 0; i < homes->event_count && c_request_poll(c); ++i)
        count += homes->events[i].kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER;
    size_t capacity = 2;
    while (capacity / 2 < count) {
        if (capacity > SIZE_MAX / 2) {
            c_error(c, XR_LIR_C_OVERFLOW, "C edge event inventory exceeds machine capacity");
            return;
        }
        capacity *= 2;
    }
    void *index = NULL;
    if (!c_request_poll(c)) return;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, capacity, sizeof(size_t), &index))) return;
    events->index = index; events->capacity = capacity;
    for (size_t i = 0; i < homes->event_count && c_request_poll(c); ++i) {
        const XrLirHomeEvent *event = &homes->events[i];
        if (event->kind != XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER) continue;
        size_t at = c_edge_event_hash(event->block, event->edge, event->target) & (capacity - 1);
        while (events->index[at]) at = (at + 1) & (capacity - 1);
        events->index[at] = i + 1;
    }
}
static bool c_edge_event(const LirCEdgeEvents *events, const XrLirHomeEdge *edge,
    XrLirHomeTransfer transfer) {
    size_t at = c_edge_event_hash(edge->block, edge->edge, transfer.target) & (events->capacity - 1);
    while (events->index[at]) {
        const XrLirHomeEvent *event = &events->homes->events[events->index[at] - 1];
        if (event->block == edge->block && event->edge == edge->edge && event->target == transfer.target)
            return event->source == transfer.source && event->target_block == edge->target_block &&
                event->transfer_mode == transfer.transfer_mode;
        at = (at + 1) & (events->capacity - 1);
    }
    return false;
}
static void c_value_frame(LirC *c, const XrLirValueHomes *homes) {
    if (!homes->owned_frame_needed) return;
    uint32_t function = homes->source_function;
    c_format(c, "static const size_t xrl_owners_%" PRIu32 "[] = {\n", function);
    for (size_t i = 0; i < homes->frame.slot_count && c_request_poll(c); ++i) {
        if (homes->frame.owned_offsets[i] == SIZE_MAX) c_text(c, "    SIZE_MAX,\n");
        else c_format(c, "    UINT64_C(%" PRIu64 "),\n", (uint64_t)homes->frame.owned_offsets[i]);
    }
    c_text(c, "};\n");
    c_format(c, "static const XrtManagedFrameSpec xrl_frame_%" PRIu32 " = {%zu, %zu, %zu, xrl_owners_%" PRIu32 "};\n",
        function, homes->frame.slot_count, homes->frame.payload_size, homes->frame.payload_alignment, function);
    c_format(c, "static XrtManagedStatus xrl_frame_%" PRIu32 "_enter(XrtManagedRoot *root, XrtManagedFrame **output, void **payload) {\n", function);
    c_text(c, "    if (!output || *output || !payload || *payload) return XRT_MANAGED_ARGUMENT;\n"
        "    XrtManagedFrame *frame = NULL;\n    void *bytes = NULL;\n    size_t extent = 0;\n");
    c_format(c, "    XrtManagedStatus status = xrt_managed_frame_enter_layout(root, &xrl_frame_%" PRIu32 ", &frame);\n", function);
    c_text(c, "    if (status != XRT_MANAGED_OK) return status;\n"
        "    status = xrt_managed_frame_payload(frame, &bytes, &extent);\n"
        "    if (status != XRT_MANAGED_OK) {\n"
        "        XrtManagedStatus cleanup = xrt_managed_frame_leave(root, frame);\n"
        "        return cleanup == XRT_MANAGED_OK ? status : cleanup;\n    }\n"
        "    *output = frame; *payload = bytes;\n    return XRT_MANAGED_OK;\n}\n");
}
static size_t c_value_edge_owners(LirC *c, const XrLirValueHomes *homes,
    const LirCEdgeEvents *events, const XrLirHomeEdge *edge) {
    size_t count = 0;
    for (size_t i = 0; i < edge->count && c_request_poll(c); ++i) {
        XrLirHomeTransfer transfer = homes->transfers[edge->first + i];
        const XrLirValueHome *target = &homes->values[transfer.target];
        if (!target->owner_count) continue;
        if (!c_edge_event(events, edge, transfer)) {
            c_error(c, XR_LIR_C_INVALID_LIR, "owned C edge has no matching final ownership event");
            return 0;
        }
        const XrLirValueHome *source = &homes->values[transfer.source];
        if (source->owner_count != target->owner_count ||
            (transfer.transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_COPY &&
             transfer.transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE)) {
            c_error(c, XR_LIR_C_INVALID_LIR, "owned C edge requires exact source leaves and a final transfer mode");
            return 0;
        }
        if (target->owner_count > SIZE_MAX - count) {
            c_error(c, XR_LIR_C_OVERFLOW, "C edge owner inventory exceeds machine capacity");
            return 0;
        }
        count += target->owner_count;
    }
    return count;
}
static void c_value_edge_parameters(LirC *c, const XrLirValueHomes *homes,
    const XrLirHomeEdge *edge, bool managed) {
    bool first = !managed;
    if (managed) c_text(c, "XrtManagedFrame *frame, void *payload, const XrtPlacePublication *scalars");
    for (size_t i = 0; i < edge->count && c_request_poll(c); ++i) {
        const XrLirValueHome *target = &homes->values[homes->transfers[edge->first + i].target];
        if (target->owner_count) continue;
        c_format(c, "%sXrlValue_%" PRIu32 " *destination_%zu, const XrlValue_%" PRIu32 " *source_%zu",
            first ? "" : ", ", target->type, i, target->type, i);
        first = false;
    }
}
static void c_value_edge_hook(LirC *c, const XrLirValueHomes *homes, const XrLirHomeEdge *edge) {
    uint32_t f = homes->source_function, b = edge->block, e = edge->edge;
    c_format(c, "typedef struct XrlEdge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " { unsigned char *payload; const XrtPlacePublication *scalars;\n", f, b, e);
    for (size_t i = 0; i < edge->count && c_request_poll(c); ++i) {
        const XrLirValueHome *target = &homes->values[homes->transfers[edge->first + i].target];
        if (!target->owner_count)
            c_format(c, "    void *destination_%zu; const void *source_%zu;\n", i, i);
    }
    c_format(c, "} XrlEdge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 ";\n", f, b, e);
    c_format(c, "static void xrl_publish_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(void *context) {\n", f, b, e);
    c_format(c, "    XrlEdge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " *edge = context;\n    (void)edge;\n", f, b, e);
    /* Incoming owner fields are installed, while all non-owner source bytes still
     * contain their old values. Stage every scalar/tag before changing any
     * destination, then clear consumed owned sources before publishing all
     * destinations. Every pass runs in the Root's existing busy interval. */
    for (unsigned pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i < edge->count && c_request_poll(c); ++i) {
            XrLirHomeTransfer transfer = homes->transfers[edge->first + i];
            const XrLirValueHome *source = &homes->values[transfer.source], *target = &homes->values[transfer.target];
            for (size_t j = 0; j < target->byte_count && c_request_poll(c); ++j) {
                XrLirHomeBytes bytes = homes->bytes[target->first_bytes + j];
                if (pass == 1) {
                    if (source->owner_count && transfer.transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE)
                        c_format(c, "    memset(edge->payload + %zu, 0, %zu);\n", source->offset + bytes.offset, bytes.size);
                    continue;
                }
                c_text(c, "    memcpy(");
                if (!pass)
                    c_format(c, "edge->payload + %zu", target->temporary_offset + bytes.offset);
                else if (target->owner_count)
                    c_format(c, "edge->payload + %zu", target->offset + bytes.offset);
                else c_format(c, "(unsigned char *)edge->destination_%zu + %zu", i, bytes.offset);
                c_text(c, ", ");
                if (pass == 2) c_format(c, "edge->payload + %zu", target->temporary_offset + bytes.offset);
                else if (source->owner_count) c_format(c, "edge->payload + %zu", source->offset + bytes.offset);
                else c_format(c, "(const unsigned char *)edge->source_%zu + %zu", i, bytes.offset);
                c_format(c, ", %zu);\n", bytes.size);
            }
        }
    }
    c_text(c, "    if (edge->scalars) edge->scalars->publish(edge->scalars->context);\n}\n");
}
static void c_value_edge(LirC *c, const XrLirValueHomes *homes,
    const LirCEdgeEvents *events, const XrLirHomeEdge *edge) {
    size_t owners = c_value_edge_owners(c, homes, events, edge);
    bool managed = owners != 0;
    uint32_t f = homes->source_function, b = edge->block, e = edge->edge;
    if (managed) c_value_edge_hook(c, homes, edge);
    c_format(c, "static %s xrl_edge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(", managed ? "XrtManagedStatus" : "void", f, b, e);
    c_value_edge_parameters(c, homes, edge, managed);
    c_text(c, ") {\n");
    if (!managed) {
        for (size_t i = 0; i < edge->count && c_request_poll(c); ++i) {
            const XrLirValueHome *target = &homes->values[homes->transfers[edge->first + i].target];
            c_format(c, "    XrlValue_%" PRIu32 " temporary_%zu = *source_%zu;\n", target->type, i, i);
        }
        for (size_t i = 0; i < edge->count && c_request_poll(c); ++i)
            c_format(c, "    *destination_%zu = temporary_%zu;\n", i, i);
        c_text(c, "}\n");
        return;
    }
    c_text(c, "    if (!frame || !payload || (scalars && !scalars->publish)) return XRT_MANAGED_ARGUMENT;\n");
    c_format(c, "    XrtPlaceEdge owners[%zu] = {\n", owners);
    for (size_t i = 0; i < edge->count && c_request_poll(c); ++i) {
        XrLirHomeTransfer transfer = homes->transfers[edge->first + i];
        const XrLirValueHome *source = &homes->values[transfer.source], *target = &homes->values[transfer.target];
        for (size_t leaf = 0; leaf < target->owner_count && c_request_poll(c); ++leaf)
            c_format(c, "        {{frame, %zu, NULL, XRT_PLACE_NULLABLE}, {frame, %zu, NULL, XRT_PLACE_NULLABLE}, {frame, %zu, NULL, XRT_PLACE_NULLABLE}, %s},\n",
                target->first_owner + leaf, source->first_owner + leaf, target->temporary_first_owner + leaf,
                transfer.transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY ? "XRT_PLACE_TRANSFER_COPY" : "XRT_PLACE_TRANSFER_MOVE");
    }
    c_text(c, "    };\n");
    c_format(c, "    XrlEdge_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " context = {payload, scalars", f, b, e);
    for (size_t i = 0; i < edge->count && c_request_poll(c); ++i)
        if (!homes->values[homes->transfers[edge->first + i].target].owner_count)
            c_format(c, ", destination_%zu, source_%zu", i, i);
    c_text(c, "};\n");
    c_format(c, "    XrtPlacePublication publication = {xrl_publish_%" PRIu32 "_%" PRIu32 "_%" PRIu32 ", &context};\n", f, b, e);
    c_format(c, "    return xrt_place_edge_transfer(owners, %zu, &publication);\n}\n", owners);
}

static void c_value_landing_type(LirC *c, LirCValueTypes *types, uint32_t type) {
    XR_CHECK(type && type <= types->count, "a managed landing has its common result type");
    if (types->rows[type - 1].landing_declared) return;
    types->rows[type - 1].landing_declared = true;
    c_format(c, "typedef struct XrlLandingValue_%" PRIu32 " {\n", type);
    c_text(c, "    XrtManagedFrame *frame;\n    size_t first_owner, temporary_first_owner;\n");
    c_format(c, "    XrlValue_%" PRIu32 " *destination, *temporary;\n} XrlLandingValue_%" PRIu32 ";\n", type, type);
}
static void c_value_landing(LirC *c, const XrLirValueHomes *homes) {
    c_value_landing_type(c,c->value_types,homes->result_type);
    c_format(c,"typedef XrlLandingValue_%" PRIu32 " XrlLanding_%" PRIu32 ";\n",
        homes->result_type,homes->source_function);
}
static void c_value_returns(LirC *c, const XrLirValueHomes *homes) {
    bool declared = false;
    for (size_t i = 0; i < homes->event_count && c_request_poll(c); ++i) {
        const XrLirHomeEvent *event = &homes->events[i];
        if (event->kind != XR_LIR_OWNERSHIP_RETURN_TRANSFER) continue;
        const XrLirValueHome *source = &homes->values[event->source];
        bool borrowed = event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY &&
            source->type == homes->result_type && !source->owner_count &&
            source->transport == XR_LIR_CALLABLE_TRANSPORT_BORROW;
        if (borrowed) {
            const XrtValuePlanSpec *spec = &c->value_types->rows[source->type - 1].spec;
            if (spec->kind != XRT_VALUE_FUNCTION || spec->owned_count != 1) {
                c_error(c,XR_LIR_C_INVALID_LIR,"borrowed callable return lost its receiving signature"); return;
            }
            if (!declared) { c_value_landing(c,homes); declared = true; }
            c_format(c,"static XrtManagedStatus xrl_return_%" PRIu32 "_%" PRIu32 "_%" PRIu32,
                homes->source_function,event->block,event->instruction);
            c_format(c,"(XrtManagedRoot *root, XrtManagedFrame *frame, void *source, XrlLanding_%" PRIu32 " landing) {\n",homes->source_function);
            c_text(c,"    if (!source || !landing.frame || !landing.destination || !landing.temporary || (frame && !root)) return XRT_MANAGED_ARGUMENT;\n");
            c_text(c,"    XrtManagedStatus status = xrt_inline_value_replace(&(XrtInlineValueHome){landing.frame, landing.destination, landing.temporary, landing.first_owner, landing.temporary_first_owner},\n");
            c_format(c,"        &(XrtInlineValueSpec){%zu, %zu, (const size_t[]){%zu}, 1}, source);\n",spec->size,spec->alignment,spec->owned_offsets[0]);
            c_text(c,"    if (status != XRT_MANAGED_OK) return status;\n    return frame ? xrt_managed_frame_leave(root,frame) : XRT_MANAGED_OK;\n}\n");
            continue;
        }
        if (source->type != homes->result_type || !source->owner_count || source->parameter ||
            event->transfer_mode != XR_LIR_OWNERSHIP_TRANSFER_MOVE) {
            c_error(c, XR_LIR_C_INVALID_LIR, "C return transfer lost its exact owned receiving home");
            return;
        }
        uint32_t f = homes->source_function, b = event->block, n = event->instruction;
        if (!declared) {
            c_value_landing(c, homes);
            declared = true;
        }
        c_format(c, "typedef struct XrlReturn_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " {\n", f, b, n);
        c_format(c, "    XrlValue_%" PRIu32 " *source;\n", source->type);
        c_format(c, "    XrlValue_%" PRIu32 " *destination, *temporary;\n} XrlReturn_%" PRIu32 "_%" PRIu32 "_%" PRIu32 ";\n", source->type, f, b, n);
        c_format(c, "static void xrl_return_publish_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(void *context) {\n", f, b, n);
        c_format(c, "    XrlReturn_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " *value = context;\n    (void)value;\n", f, b, n);
        for (unsigned pass = 0; pass < 3; ++pass)
            for (size_t j = 0; j < source->byte_count && c_request_poll(c); ++j) {
                XrLirHomeBytes bytes = homes->bytes[source->first_bytes + j];
                if (pass == 1) {
                    c_format(c, "    memset((unsigned char *)value->source + %zu, 0, %zu);\n", bytes.offset, bytes.size);
                    continue;
                }
                c_format(c, "    memcpy((unsigned char *)value->%s + %zu, (const unsigned char *)value->%s + %zu, %zu);\n",
                    pass ? "destination" : "temporary", bytes.offset, pass ? "temporary" : "source", bytes.offset, bytes.size);
            }
        c_text(c, "}\n");
        c_format(c, "static XrtManagedStatus xrl_return_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "(XrtManagedRoot *root, XrtManagedFrame *frame, void *payload, XrlLanding_%" PRIu32 " landing) {\n", f, b, n, f);
        c_text(c, "    if (!root || !frame || !payload || !landing.frame || !landing.destination || !landing.temporary)\n"
            "        return XRT_MANAGED_ARGUMENT;\n");
        c_format(c, "    XrtPlaceEdge owners[%zu] = {\n", source->owner_count);
        for (size_t leaf = 0; leaf < source->owner_count && c_request_poll(c); ++leaf)
            c_format(c, "        {{landing.frame, landing.first_owner + %zu, NULL, XRT_PLACE_NULLABLE}, {frame, %zu, NULL, XRT_PLACE_NULLABLE}, {landing.frame, landing.temporary_first_owner + %zu, NULL, XRT_PLACE_NULLABLE}, XRT_PLACE_TRANSFER_MOVE},\n",
                leaf, source->first_owner + leaf, leaf);
        c_text(c, "    };\n");
        c_format(c, "    XrlReturn_%" PRIu32 "_%" PRIu32 "_%" PRIu32 " context = {(void *)((unsigned char *)payload + %zu), landing.destination, landing.temporary};\n",
            f, b, n, source->offset);
        c_format(c, "    XrtPlacePublication publication = {xrl_return_publish_%" PRIu32 "_%" PRIu32 "_%" PRIu32 ", &context};\n", f, b, n);
        c_format(c, "    XrtManagedStatus status = xrt_place_edge_transfer(owners, %zu, &publication);\n", source->owner_count);
        c_text(c, "    if (status != XRT_MANAGED_OK) return status;\n"
            "    return xrt_managed_frame_leave(root, frame);\n}\n");
    }
}

XR_FUNC XrLirCStatus xr_lir_emit_c_value_transfers(const XrLirCValueTransfersRequest *request,
    XrLirCOutput *output, XrLirCDiagnostic *diagnostic) {
    if (diagnostic) *diagnostic = (XrLirCDiagnostic){0};
    XrLirCRequest common = {0};
    LirC c = {.request=&common, .diagnostic=diagnostic};
    if (!request || !request->layouts || !request->homes || !request->resources || !request->translation_unit ||
        !output || output->bytes || output->length || output->entry_result || output->entry_shape.kind ||
        output->entry_shape.element || output->entry_shape.count || output->entry_layout.size ||
        output->entry_layout.alignment || output->entry_layout.scalar_count || output->entry_layout.managed) {
        c_error(&c, XR_LIR_C_BAD_ARGUMENT, "C value transfers require common homes, plans, resources and empty output");
        return c.status;
    }
    common.resources = request->resources; common.translation_unit = request->translation_unit;
    common.control = request->control;
    const XrLirValueHomes *homes = request->homes;
    c.function.id = homes->source_function;
    if (homes->unmodeled) {
        c_error(&c, XR_LIR_C_UNSUPPORTED, "C value transfers require modeled ownership without capabilities or callable lifetimes");
        return c.status;
    }
    LirCValueTypes types = {0}; LirCEdgeEvents events = {0};
    if (c_request_poll(&c)) c_value_types_prepare(&c, request->layouts, &types);
    c.value_types = &types;
    if (c_request_poll(&c)) c_edge_events_prepare(&c, homes, &events);
    c_text(&c, "#include <stddef.h>\n#include <stdint.h>\n");
    for (size_t i = 0; i < types.count && c_request_poll(&c); ++i)
        if (types.rows[i].spec.kind == XRT_VALUE_FUNCTION) {
            c_text(&c, "#include \"xray_native_function.h\"\n");
            break;
        }
    if (homes->owned_frame_needed) c_text(&c, "#include <string.h>\n#include \"xray_native_place.h\"\n");
    else c_text(&c, "typedef struct XrtManagedHeader XrtManagedHeader;\n");
    for (size_t i = 0; i < types.count && c_request_poll(&c); ++i)
        c_value_type_declaration(&c, &types, i);
    c_value_frame(&c, homes);
    for (size_t i = 0; i < homes->edge_count && c_request_poll(&c); ++i) {
        c.block = homes->edges[i].block;
        c_value_edge(&c, homes, &events, &homes->edges[i]);
    }
    c_value_returns(&c, homes);
    c_verify_text(&c);
    if (c_request_poll(&c)) {
        *output = (XrLirCOutput){.bytes=c.text, .length=c.length};
        c.text = NULL;
    }
    xr_compile_resources_free(c.text);
    xr_compile_resources_free(events.index);
    xr_compile_resources_free(types.index);
    xr_compile_resources_free(types.rows);
    return c.status;
}
