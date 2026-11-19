/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_inline_array_return_fixture.h - Owner transport through an ordinary call
 */
static XrLirModule *inline_array_return_fixture(XrLirFunction *root_out, XrLirFunction *pack_out) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    XrLirRecordField fields[] = {{.type=XR_LIR_STRING}, {.type=XR_LIR_I64}};
    OK(xr_lir_tuple_type(c.module, fields, 2, &c.element));
    OK(xr_lir_require_inline_array_element(c.module, c.element));
    OK(xr_lir_array_catalog_install(c.module));
    XrLirFunction root = {0}, pack = {0};
    XrLirFunctionSpec spec = {.name="root", .name_length=4, .result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, &root));
    spec = (XrLirFunctionSpec){.name="pack", .name_length=4, .result=XR_LIR_ARRAY};
    OK(xr_lir_declare_function(c.module, &spec, &pack));
    OK(xr_lir_set_array_result(c.module, pack, &(XrLirArrayShape){c.element}));
    OK(xr_lir_define_function(c.module, pack));
    XrLirBlock body = {0};
    OK(xr_lir_add_block(c.module, pack, NULL, 0, &body));
    OK(xr_lir_set_entry(c.module, pack, body));
    XrLirValue values[] = {ca_text(&c, body, "owner", 5), ca_constant(&c, body, 42)};
    XrLirValue tuple = ca_value(&c, body, XR_LIR_RECORD_NEW, c.element, values, 2);
    ca_effect(&c, body, XR_LIR_STRING_DROP, values[0]);
    XrLirValue array = ca_intrinsic(&c, body, XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY, &tuple, 1);
    ca_effect(&c, body, XR_LIR_VALUE_DROP, tuple);
    ca_return(&c, body, array);

    OK(xr_lir_define_function(c.module, root));
    XrLirBlock entry = {0};
    OK(xr_lir_add_block(c.module, root, NULL, 0, &entry));
    OK(xr_lir_set_entry(c.module, root, entry));
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_ARRAY, .callee=pack, .source=ca_source()};
    OK(xr_lir_emit_value(c.module, entry, &call, &array));
    XrLirValue get[] = {array, ca_constant(&c, entry, 0)};
    tuple = ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_GET, c.element, get, 2);
    ca_effect(&c, entry, XR_LIR_ARRAY_DROP, array);
    XrLirValue text = ca_value(&c, entry, XR_LIR_RECORD_FIELD, XR_LIR_STRING, &tuple, 1);
    ca_effect(&c, entry, XR_LIR_VALUE_DROP, tuple);
    XrLirValue length = ca_value(&c, entry, XR_LIR_STRING_LEN, XR_LIR_I64, &text, 1);
    ca_effect(&c, entry, XR_LIR_STRING_DROP, text);
    ca_return(&c, entry, length);
    OK(xr_lir_module_finish(c.module, NULL));

    *root_out = root;
    *pack_out = pack;
    return c.module;
}
