/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_map_fixture.h - Typed Map owners, mutable borrows and selected-edge spills
 *
 * KEY CONCEPT:
 *   Real typed constructors and borrows feed the same compiler and runtime
 *   paths used by Source, including managed values on selected edges.
 */
#include "lir/xlir_map.h"
static XrLirValue map_fixture_call(CArrayFixture *c, XrLirBlock block, XrLirMapOperation role,
    XrLirType result, const XrLirValue *args, uint32_t count) {
    XrLirType receiver = role == XR_LIR_MAP_OP_SET ? XR_LIR_MAP_STRING_I64_REF : XR_LIR_MAP_STRING_I64;
    uint32_t id = 0;
    OK(xr_lir_map_operation(c->module, role, receiver, &id));
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC, .type=result, .intrinsic=id,
        .operands=args, .operand_count=count};
    XrLirValue value = {0};
    if (result == XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module, block, &call));
    else OK(xr_lir_emit_value(c->module, block, &call, &value));
    return value;
}
static XrLirModule *map_fixture(uint32_t copies) {
    CHECK(copies && copies <= 300);
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_map_catalog_install(c.module, false, false, false, false, false));
    XrLirType nullable = 0, map_type = XR_LIR_MAP_STRING_I64;
    OK(xr_lir_nullable_type(c.module, XR_LIR_I64, &nullable));
    XrLirFunction root = {0};
    XrLirFunctionSpec spec = {.name="map_root", .name_length=8, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &root));
    OK(xr_lir_define_function(c.module, root));
    XrLirBlock entry = {0}, join = {0};
    OK(xr_lir_add_block(c.module, root, NULL, 0, &entry));
    XrLirType join_types[] = {map_type, XR_LIR_STRING};
    OK(xr_lir_add_block(c.module, root, join_types, 2, &join));
    OK(xr_lir_set_entry(c.module, root, entry));
    XrLirValue key = ca_text(&c, entry, "answer", 6);
    XrLirValue args[] = {key, ca_constant(&c, entry, 40)};
    XrLirValue initial = map_fixture_call(&c, entry, XR_LIR_MAP_OP_NEW, map_type, args, 2);
    XrLirValue cell = ca_value(&c, entry, XR_LIR_CELL_NEW, XR_LIR_MAP_STRING_I64_CELL, &initial, 1);
    ca_effect(&c, entry, XR_LIR_MAP_DROP, initial);
    XrLirValue ref = ca_value(&c, entry, XR_LIR_MAP_BORROW, XR_LIR_MAP_STRING_I64_REF, &cell, 1);
    XrLirValue write[] = {ref, key, ca_constant(&c, entry, 42)};
    (void)map_fixture_call(&c, entry, XR_LIR_MAP_OP_SET, XR_LIR_UNIT, write, 3);
    XrLirValue read = ca_value(&c, entry, XR_LIR_MAP_REF_READ, map_type, &ref, 1);
    ca_effect(&c, entry, XR_LIR_MAP_CELL_DROP, cell);
    XrLirValue owners[300];
    for (uint32_t n = 0; n < copies; ++n) owners[n] = ca_value(&c, entry, XR_LIR_MAP_RETAIN, map_type, &read, 1);
    ca_effect(&c, entry, XR_LIR_MAP_DROP, read);
    for (uint32_t n = 0; n + 1 < copies; ++n) ca_effect(&c, entry, XR_LIR_MAP_DROP, owners[n]);
    XrLirValue condition = {0};
    XrLirInstruction constant = {.op=XR_LIR_CONST, .type=XR_LIR_I1, .bits=1};
    OK(xr_lir_emit_value(c.module, entry, &constant, &condition));
    XrLirValue outgoing[] = {owners[copies - 1], key};
    XrLirInstruction branch = {.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&condition, .operand_count=1,
        .edge_count=2, .edges={{join, outgoing, 2}, {join, outgoing, 2}}};
    OK(xr_lir_set_terminator(c.module, entry, &branch));
    XrLirValue selected = {0};
    OK(xr_lir_block_parameter(c.module, join, 0, &selected));
    OK(xr_lir_block_parameter(c.module, join, 1, &key));
    XrLirValue get[] = {selected, key};
    XrLirValue optional = map_fixture_call(&c, join, XR_LIR_MAP_OP_GET, nullable, get, 2);
    ca_effect(&c, join, XR_LIR_MAP_DROP, selected);
    ca_effect(&c, join, XR_LIR_STRING_DROP, key);
    ca_return(&c, join, ca_value(&c, join, XR_LIR_NULLABLE_UNWRAP, XR_LIR_I64, &optional, 1));
    XrLirDiagnostic diagnostic = {0};
    XrLirStatus status = xr_lir_module_finish(c.module, &diagnostic);
    if (status != XR_LIR_OK) fprintf(stderr, "Map finish f%u b%u i%u: %s\n", diagnostic.function,
        diagnostic.block, diagnostic.instruction, diagnostic.message);
    OK(status);
    return c.module;
}

static XrLirModule *map_state_fixture(void) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_map_catalog_install(c.module, false, false, false, false, false));
    XrLirStateModule owner = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_MAP_STRING_I64, .mutable=true, .size=8, .alignment=8};
    XrLirStateLayout layout = {.modules=&owner, .module_count=1, .slots=&slot, .slot_count=1,
        .payload_size=8, .payload_alignment=8};
    OK(xr_lir_set_state_layout(c.module, &layout));
    XrLirType state_type = XR_LIR_STATE, nullable = 0;
    OK(xr_lir_nullable_type(c.module, XR_LIR_I64, &nullable));
    XrLirFunction root = {0};
    XrLirFunctionSpec spec = {.name="map_state", .name_length=9, .parameters=&state_type,
        .parameter_count=1, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &root));
    OK(xr_lir_set_state_entry(c.module, root));
    OK(xr_lir_define_function(c.module, root));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module, root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, root, block));
    XrLirValue state = {0};
    OK(xr_lir_function_parameter(c.module, root, 0, &state));
    ca_effect(&c, block, XR_LIR_MODULE_BEGIN, state);
    XrLirValue key = ca_text(&c, block, "answer", 6);
    XrLirValue args[] = {key, ca_constant(&c, block, 42)};
    XrLirValue map = map_fixture_call(&c, block, XR_LIR_MAP_OP_NEW, XR_LIR_MAP_STRING_I64, args, 2);
    XrLirValue construct[] = {state, map};
    XrLirValue initial = ca_value(&c, block, XR_LIR_STATE_CONSTRUCT, XR_LIR_STATE_REF, construct, 2);
    ca_effect(&c, block, XR_LIR_STATE_PUBLISH, initial);
    ca_effect(&c, block, XR_LIR_MODULE_READY, state);
    XrLirValue reference = ca_value(&c, block, XR_LIR_STATE_REFERENCE, XR_LIR_STATE_REF, &state, 1);
    XrLirValue write[] = {reference, map};
    XrLirInstruction replace = {.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT, .operands=write, .operand_count=2};
    OK(xr_lir_emit_effect(c.module, block, &replace));
    XrLirValue loaded = ca_value(&c, block, XR_LIR_STATE_READ, XR_LIR_MAP_STRING_I64, &reference, 1);
    ca_effect(&c, block, XR_LIR_MAP_DROP, map);
    XrLirValue get[] = {loaded, key};
    XrLirValue optional = map_fixture_call(&c, block, XR_LIR_MAP_OP_GET, nullable, get, 2);
    ca_effect(&c, block, XR_LIR_MAP_DROP, loaded);
    ca_effect(&c, block, XR_LIR_STRING_DROP, key);
    ca_return(&c, block, ca_value(&c, block, XR_LIR_NULLABLE_UNWRAP, XR_LIR_I64, &optional, 1));
    OK(xr_lir_module_finish(c.module, NULL));
    return c.module;
}
