/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_C_MAP_LITERAL_FIXTURE_H
#define LIR_C_MAP_LITERAL_FIXTURE_H
/* Modes: explicit entries, spread, empty, duplicate, unknown-path duplicate,
 * and a legal spread overwriting an earlier explicit value. */
static void c_map_literal_fixture(unsigned mode, XrLirModule **output, XrLirFunction *root) {
    CArrayFixture c = {0}; OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_map_catalog_install(c.module, false, false, false, false, false));
    OK(xr_lir_declare_function(c.module, &(XrLirFunctionSpec){.name="literal", .name_length=7,
        .result=XR_LIR_I64}, root)); OK(xr_lir_define_function(c.module, *root));
    XrLirBlock block = {0}; OK(xr_lir_add_block(c.module, *root, NULL, 0, &block));
    OK(xr_lir_set_entry(c.module, *root, block));
    char path[] = "map-\xc3\xa9.xr";
    XrLirMapLiteralOrigin origins[] = {{0,3,12}, {0,3,20}};
    XrLirMapLiteral literal = {.path=path, .path_length=(uint32_t)sizeof(path) - 1,
        .items=origins, .count=2};
    if (mode == 4) { literal.path = NULL; literal.path_length = 0; }
    uint32_t unused = 0, ordinal = 0;
    OK(xr_lir_map_literal_add(c.module, &literal, &unused));
    CHECK(unused == 1);
    if (mode == 2) { literal.items = NULL; literal.count = 0; }
    OK(xr_lir_map_literal_add(c.module, &literal, &ordinal)); CHECK(ordinal == 2);
    XrLirValue keys[2] = {{0}}, arguments[4] = {{0}};
    if (mode != 2) {
        keys[0] = ca_text(&c, block, "x", 1); keys[1] = ca_text(&c, block, "y", 1);
        arguments[0] = keys[0]; arguments[1] = ca_constant(&c, block, 4);
        arguments[2] = keys[1]; arguments[3] = ca_constant(&c, block, 2);
    }
    XrLirInstruction instruction = {.op=XR_LIR_MAP_LITERAL_NEW, .type=XR_LIR_MAP_STRING_I64,
        .bits=ordinal, .operands=mode == 2 ? NULL : arguments, .operand_count=mode == 2 ? 0u : 4u,
        .source=ca_source()};
    XrLirValue base = {0}, map = {0};
    OK(xr_lir_emit_value(c.module, block, &instruction, &base)); map = base;
    if (mode == 1 || mode >= 3) {
        XrLirMapLiteralOrigin merged[] = {{0,3,12}, {1,4,8}, {0,5,16}};
        literal.items = mode == 1 ? &merged[1] : merged;
        literal.count = mode == 1 ? 1u : mode == 5 ? 2u : 3u;
        OK(xr_lir_map_literal_add(c.module, &literal, &ordinal)); CHECK(ordinal == 3);
        XrLirValue inputs[] = {keys[0], ca_constant(&c, block, 9), base,
            keys[0], ca_constant(&c, block, 7)};
        instruction.bits = ordinal; instruction.operands = mode == 1 ? &base : inputs;
        instruction.operand_count = mode == 1 ? 1u : mode == 5 ? 3u : 5u;
        OK(xr_lir_emit_value(c.module, block, &instruction, &map));
    }
    path[0] = 'z'; origins[0].line = 99;
    XrLirValue answer = {0};
    if (mode == 2) answer = map_fixture_call(&c, block, XR_LIR_MAP_OP_LEN, XR_LIR_I64, &map, 1);
    else {
        XrLirValue index[] = {map, keys[0]};
        XrLirValue left = map_fixture_call(&c, block, XR_LIR_MAP_OP_INDEX, XR_LIR_I64, index, 2);
        index[1] = keys[1];
        XrLirValue right = map_fixture_call(&c, block, XR_LIR_MAP_OP_INDEX, XR_LIR_I64, index, 2);
        left = ca_binary(&c, block, XR_LIR_MUL, left, ca_constant(&c, block, 10));
        answer = ca_binary(&c, block, XR_LIR_ADD, left, right);
    }
    ca_effect(&c, block, XR_LIR_MAP_DROP, map);
    if (map.id != base.id) ca_effect(&c, block, XR_LIR_MAP_DROP, base);
    if (mode != 2) {
        ca_effect(&c, block, XR_LIR_STRING_DROP, keys[0]); ca_effect(&c, block, XR_LIR_STRING_DROP, keys[1]);
    }
    ca_return(&c, block, answer); OK(xr_lir_module_finish(c.module, NULL)); *output = c.module;
}
#endif // LIR_C_MAP_LITERAL_FIXTURE_H
