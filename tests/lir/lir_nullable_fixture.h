/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_nullable_fixture.h - A real typed nullable result under one SSA owner
 */
#ifndef LIR_NULLABLE_FIXTURE_H
#define LIR_NULLABLE_FIXTURE_H
#include "lir/xlir_types.h"

static XrLirModule *nullable_none_module(uint32_t depth, XrLirType leaf, XrLirFunction *root) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType type = leaf;
    CHECK(depth && leaf < XR_LIR_TYPE_COUNT);
    for (uint32_t n = 0; n < depth; ++n) OK(xr_lir_nullable_type(module, type, &type));
    *root = test_function(module, "nullable_none", NULL, 0, type);
    XrLirBlock block = test_block(module, *root, NULL, 0);
    OK(xr_lir_set_entry(module, *root, block));
    XrLirValue value = {0};
    XrLirInstruction none = {.op=XR_LIR_NULLABLE_NONE, .type=type};
    OK(xr_lir_emit_value(module, block, &none, &value));
    test_return(module, block, value);
    OK(xr_lir_module_finish(module, NULL));
    return module;
}
#endif // LIR_NULLABLE_FIXTURE_H
