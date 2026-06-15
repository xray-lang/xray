/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_bytecode_io.c - Bytecode file-format roundtrip tests
 */

#include "../test_framework.h"

#include "base/xmalloc.h"
#include "module/xbytecode_io.h"
#include "runtime/xisolate_api.h"
#include "runtime/symbol/xsymbol_table.h"
#include "runtime/value/xchunk.h"
#include "xray_isolate.h"

static XrayIsolate *new_test_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t) p[0] | (uint16_t) ((uint16_t) p[1] << 8);
}

static XrProto *make_minimal_proto(void) {
    XrProto *proto = xr_vm_proto_new();
    if (!proto)
        return NULL;
    proto->source_file = "<bytecode-io-test>";
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 0, 0), 1);
    return proto;
}

TEST(bytecode_write_emits_v5_header_and_roundtrips_u64_instruction) {
    XrayIsolate *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);
    ASSERT_GT(size, 16);
    ASSERT_EQ_UINT(read_le16(bytes + 4), XR_BC_VERSION);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CODE_COUNT(roundtrip), 1);
    ASSERT_EQ_INT(GET_OPCODE(PROTO_CODE(roundtrip, 0)), OP_RETURN);
    ASSERT_EQ_INT(roundtrip->maxstacksize, 1);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_isolate_delete(iso);
}

TEST(bytecode_reader_rejects_previous_layout_version) {
    XrayIsolate *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);
    ASSERT_GT(size, 16);

    uint16_t previous_version = (uint16_t) (XR_BC_VERSION - 1);
    bytes[4] = (uint8_t) (previous_version & 0xff);
    bytes[5] = (uint8_t) (previous_version >> 8);

    XrBcError error = XR_BC_OK;
    XrProto *bad = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NULL(bad);
    ASSERT_EQ_INT(error, XR_BC_ERR_VERSION);

    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_isolate_delete(iso);
}

TEST(bytecode_roundtrips_u16_upvalue_index) {
    XrayIsolate *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_vm_proto_add_upvalue(proto, 300, 0, 1, 0, UPVAL_SRC_REG, NULL), 0);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);
    ASSERT_GT(size, 16);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_UPVAL_COUNT(roundtrip), 1);

    UpvalInfo info = PROTO_UPVALUE(roundtrip, 0);
    ASSERT_EQ_UINT(info.index, 300);
    ASSERT_EQ_UINT(info.source, UPVAL_SRC_REG);
    ASSERT_EQ_UINT(info.is_const, 1);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_isolate_delete(iso);
}

TEST(bytecode_roundtrips_symbol_index_above_255) {
    XrayIsolate *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_vm_proto_new();
    ASSERT_NOT_NULL(proto);
    proto->source_file = "<bytecode-symbol-test>";
    proto->maxstacksize = 2;

    XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(iso);
    ASSERT_NOT_NULL(st);

    char name[32];
    int local_idx = -1;
    for (int i = 0; i <= 300; i++) {
        snprintf(name, sizeof(name), "wide_symbol_%03d", i);
        SymbolId sym = xr_symbol_register_in_table(st, name);
        ASSERT_TRUE(sym > 0);
        local_idx = xr_proto_add_symbol(proto, (int32_t) sym);
        ASSERT_EQ_INT(local_idx, i);
    }
    ASSERT_EQ_INT(local_idx, 300);
    ASSERT_EQ_INT(PROTO_SYMBOL_COUNT(proto), 301);
    xr_vm_proto_write(proto, CREATE_ABC(OP_GETPROP, 1, 0, local_idx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 1, 2, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_SYMBOL_COUNT(roundtrip), 301);
    ASSERT_EQ_INT(GET_OPCODE(PROTO_CODE(roundtrip, 0)), OP_GETPROP);
    ASSERT_EQ_UINT(GETARG_C(PROTO_CODE(roundtrip, 0)), 300);

    const char *roundtrip_name =
        xr_symbol_get_name_in_table(st, (SymbolId) PROTO_SYMBOL(roundtrip, 300));
    ASSERT_NOT_NULL(roundtrip_name);
    ASSERT_STR_EQ(roundtrip_name, "wide_symbol_300");

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_isolate_delete(iso);
}

static void run_all_tests(void) {
    RUN_TEST_SUITE("Bytecode I/O");
    RUN_TEST(bytecode_write_emits_v5_header_and_roundtrips_u64_instruction);
    RUN_TEST(bytecode_reader_rejects_previous_layout_version);
    RUN_TEST(bytecode_roundtrips_u16_upvalue_index);
    RUN_TEST(bytecode_roundtrips_symbol_index_above_255);
}

TEST_MAIN_BEGIN()
printf("=== xray Bytecode I/O Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
