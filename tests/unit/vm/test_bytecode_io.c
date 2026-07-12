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
#include "runtime/class/xclass.h"
#include "runtime/class/xclass_descriptor.h"
#include "runtime/class/xenum.h"
#include "runtime/class/xinstance.h"
#include "runtime/object/xstring.h"
#include "runtime/symbol/xsymbol_table.h"
#include "runtime/value/xchunk.h"
#include "runtime/value/xffi_sig.h"
#include "xray_vm.h"

static XrVMRuntime *new_test_isolate(void) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    return xray_vm_new_full(&params);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t) p[0] | (uint16_t) ((uint16_t) p[1] << 8);
}

static XrProto *make_minimal_proto(void) {
    XrProto *proto = xr_vm_proto_new();
    if (!proto)
        return NULL;
    /* XrProto owns source_file (freed in xr_vm_proto_free), so it must be
     * heap-allocated — never a string literal. */
    proto->source_file = xr_strdup("<bytecode-io-test>");
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 0, 0), 1);
    return proto;
}

TEST(bytecode_write_emits_current_header_and_roundtrips_u64_instruction) {
    XrVMRuntime *iso = new_test_isolate();
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
    ASSERT_EQ_UINT(roundtrip->entry_plan.entry_func_id, 1);
    ASSERT_EQ_UINT(roundtrip->entry_plan.root_representation, XR_ROOT_ELIDED);
    ASSERT_EQ_UINT(roundtrip->entry_plan.scheduler_mode, XR_SCHED_NONE);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_reachable_entry_plan) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *root = make_minimal_proto();
    XrProto *child = make_minimal_proto();
    ASSERT_NOT_NULL(root);
    ASSERT_NOT_NULL(child);
    child->code.count = 0;
    child->lineinfo.count = 0;
    xr_vm_proto_write(child, CREATE_ABC(OP_GO, 0, 0, 0), 1);
    xr_vm_proto_write(child, CREATE_ABC(OP_RETURN, 0, 0, 0), 1);
    ASSERT_EQ_INT(xr_vm_proto_add_proto(root, child), 0);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, root, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_UINT(roundtrip->entry_plan.entry_func_id, 1);
    ASSERT_EQ_UINT(roundtrip->entry_plan.reachable_body_count, 2);
    ASSERT_TRUE((roundtrip->entry_plan.required_capability_bits & XR_CAP_COROUTINE) != 0);
    ASSERT_TRUE((roundtrip->entry_plan.required_capability_bits & XR_CAP_TASK) != 0);
    ASSERT_EQ_UINT(roundtrip->entry_plan.root_representation, XR_ROOT_DESCRIPTOR);
    ASSERT_EQ_UINT(roundtrip->entry_plan.scheduler_mode, XR_SCHED_SINGLE);
    ASSERT_TRUE(xr_vm_entry_plan_validate(roundtrip));

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(root);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_struct_area_size) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    proto->struct_area_size = 48;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_UINT(roundtrip->struct_area_size, 48);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_exact_string_constant_lengths) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    XrString *empty = xr_string_intern(writer, "", 0, 0);
    ASSERT_NOT_NULL(empty);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, xr_string_value(empty)), 0);
    const char embedded_nul[] = {'a', '\0', 'b'};
    XrString *binary = xr_string_intern(writer, embedded_nul, sizeof(embedded_nul), 0);
    ASSERT_NOT_NULL(binary);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, xr_string_value(binary)), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 2);
    XrValue value = PROTO_CONSTANT(roundtrip, 0);
    ASSERT_TRUE(XR_IS_STRING(value));
    ASSERT_EQ_UINT(XR_TO_STRING(value)->length, 0);
    ASSERT_STR_EQ(XR_TO_STRING(value)->data, "");
    value = PROTO_CONSTANT(roundtrip, 1);
    ASSERT_TRUE(XR_IS_STRING(value));
    ASSERT_EQ_UINT(XR_TO_STRING(value)->length, sizeof(embedded_nul));
    ASSERT_TRUE(memcmp(XR_TO_STRING(value)->data, embedded_nul, sizeof(embedded_nul)) == 0);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_reader_assigns_unique_proto_ids) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    XrProto *child = make_minimal_proto();
    ASSERT_NOT_NULL(child);
    ASSERT_EQ_INT(xr_vm_proto_add_proto(proto, child), 0);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_PROTO_COUNT(roundtrip), 1);

    XrProto *roundtrip_child = PROTO_PROTO(roundtrip, 0);
    ASSERT_NOT_NULL(roundtrip_child);
    ASSERT_TRUE(roundtrip->proto_id != roundtrip_child->proto_id);
    ASSERT_TRUE(roundtrip_child->enclosing == roundtrip);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_reader_rejects_previous_layout_version) {
    XrVMRuntime *iso = new_test_isolate();
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
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_dynamic_json_shape_across_isolates) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    const char *names[] = {"host", "port"};
    XrClass *shape = xr_class_build_json_chain(writer, names, 2, false);
    ASSERT_NOT_NULL(shape);

    int kidx = xr_valuearray_add(&proto->constants, xr_int((int64_t) (intptr_t) shape));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABC(OP_NEWJSON, 0, kidx, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 1);

    XrValue cls_val = PROTO_CONSTANT(roundtrip, 0);
    ASSERT_TRUE(XR_IS_INT(cls_val));
    XrClass *roundtrip_shape = (XrClass *) (intptr_t) XR_TO_INT(cls_val);
    ASSERT_NOT_NULL(roundtrip_shape);
    ASSERT_EQ_UINT(roundtrip_shape->builtin_kind, XR_BK_JSON);
    ASSERT_TRUE((roundtrip_shape->flags & XR_CLASS_DYNAMIC_LAYOUT) != 0);
    ASSERT_EQ_UINT(roundtrip_shape->field_count, 2);
    ASSERT_STR_EQ(roundtrip_shape->fields[0].name, "host");
    ASSERT_STR_EQ(roundtrip_shape->fields[1].name, "port");

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_class_descriptor_constants) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    XrClassDescriptor *desc = xr_calloc(1, sizeof(XrClassDescriptor));
    ASSERT_NOT_NULL(desc);
    desc->class_name = "BytecodeClass";
    desc->display_name = "";
    desc->descriptor_version = XR_CLASS_DESCRIPTOR_VERSION;
    desc->clinit_proto_index = -1;
    desc->super_global_index = -1;
    desc->instance_field_count = 1;
    desc->instance_fields = xr_calloc(1, sizeof(XrFieldDescriptorEntry));
    ASSERT_NOT_NULL(desc->instance_fields);
    desc->instance_fields[0].name = "value";
    desc->instance_fields[0].type_name = "Int";
    desc->instance_fields[0].default_value = xr_int(42);

    int kidx = xr_valuearray_add(&proto->constants, XR_FROM_PTR(desc));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, kidx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 1);

    XrValue roundtrip_val = PROTO_CONSTANT(roundtrip, 0);
    ASSERT_TRUE(XR_IS_PTR(roundtrip_val));
    XrClassDescriptor *roundtrip_desc = XR_TO_PTR(roundtrip_val);
    ASSERT_NOT_NULL(roundtrip_desc);
    ASSERT_STR_EQ(roundtrip_desc->class_name, "BytecodeClass");
    ASSERT_NULL(roundtrip_desc->super_name);
    ASSERT_NOT_NULL(roundtrip_desc->display_name);
    ASSERT_STR_EQ(roundtrip_desc->display_name, "");
    ASSERT_EQ_INT(roundtrip_desc->clinit_proto_index, -1);
    ASSERT_EQ_INT(roundtrip_desc->super_global_index, -1);
    ASSERT_EQ_UINT(roundtrip_desc->descriptor_version, XR_CLASS_DESCRIPTOR_VERSION);
    ASSERT_EQ_UINT(roundtrip_desc->instance_field_count, 1);
    ASSERT_STR_EQ(roundtrip_desc->instance_fields[0].name, "value");
    ASSERT_STR_EQ(roundtrip_desc->instance_fields[0].type_name, "Int");
    ASSERT_TRUE(XR_IS_INT(roundtrip_desc->instance_fields[0].default_value));
    ASSERT_EQ_INT(XR_TO_INT(roundtrip_desc->instance_fields[0].default_value), 42);

    xr_free((void *) roundtrip_desc->instance_fields[0].name);
    xr_free((void *) roundtrip_desc->instance_fields[0].type_name);
    xr_free(roundtrip_desc->instance_fields);
    xr_free((void *) roundtrip_desc->class_name);
    xr_free((void *) roundtrip_desc->display_name);
    xr_free(roundtrip_desc);
    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_free(desc->instance_fields);
    xr_free(desc);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_enum_type_constants) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    char *members[] = {"Ok", "Err"};
    XrEnumType *enum_type = xr_enum_type_new(writer, "BytecodeResult", members, 2);
    ASSERT_NOT_NULL(enum_type);
    int payload_counts[] = {0, 1};
    ASSERT_TRUE(xr_enum_type_set_adt_payloads(enum_type, payload_counts, 2));
    enum_type->derive_flags = XR_DERIVE_INSPECT | XR_DERIVE_EQ;

    int kidx = xr_valuearray_add(&proto->constants, XR_FROM_PTR(enum_type));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABx(OP_LOADK, 0, kidx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 1);

    XrValue roundtrip_val = PROTO_CONSTANT(roundtrip, 0);
    ASSERT_TRUE(XR_IS_ENUM_TYPE(roundtrip_val));
    XrEnumType *roundtrip_enum = XR_TO_ENUM_TYPE(roundtrip_val);
    ASSERT_NOT_NULL(roundtrip_enum);
    ASSERT_STR_EQ(roundtrip_enum->name, "BytecodeResult");
    ASSERT_EQ_UINT(roundtrip_enum->member_count, 2);
    ASSERT_STR_EQ(xr_enum_type_member_name(roundtrip_enum, 0), "Ok");
    ASSERT_STR_EQ(xr_enum_type_member_name(roundtrip_enum, 1), "Err");
    ASSERT_EQ_INT(xr_enum_type_payload_count(roundtrip_enum, 0), 0);
    ASSERT_EQ_INT(xr_enum_type_payload_count(roundtrip_enum, 1), 1);
    ASSERT_EQ_UINT(roundtrip_enum->derive_flags, XR_DERIVE_INSPECT | XR_DERIVE_EQ);

    XrSymbolTable *reader_symbols = (XrSymbolTable *) xr_isolate_get_symbol_table(reader);
    ASSERT_NOT_NULL(reader_symbols);
    SymbolId err_sym = xr_symbol_lookup_in_table(reader_symbols, "Err");
    ASSERT_TRUE(err_sym > 0);
    ASSERT_EQ_INT(xr_enum_type_find_member_index_by_symbol(roundtrip_enum, err_sym), 1);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_u16_upvalue_index) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(
        xr_vm_proto_add_upvalue(proto, 300, 0, 1, 0, UPVAL_SRC_REG, XR_CAPTURE_DEEP_COPY, NULL), 0);

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
    ASSERT_EQ_UINT(info.capture_action, XR_CAPTURE_DEEP_COPY);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_declared_shared_count) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    proto->shared_count = 4;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(roundtrip->shared_count, 4);
    ASSERT_FALSE(roundtrip->shared_slots_bound);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_symbol_index_above_255) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = xr_vm_proto_new();
    ASSERT_NOT_NULL(proto);
    /* XrProto owns source_file (freed in xr_vm_proto_free); use a heap copy. */
    proto->source_file = xr_strdup("<bytecode-symbol-test>");
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
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_extern_ffi_signature) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    /* @extern fn pow(base: float64, exp: float64) -> float64 @dylib("m").
     * The Xi IR is not serialized into embedded bytecode, so the FFI signature
     * must round-trip on the proto for the libffi invoker to work. */
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    proto->is_extern = true;
    XrFFISig *sig = xr_ffi_sig_new("pow", "m", 2);
    ASSERT_NOT_NULL(sig);
    sig->params[0] = XR_FFI_T_F64;
    sig->params[1] = XR_FFI_T_F64;
    sig->ret = XR_FFI_T_F64;
    proto->ffi_sig = sig;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_TRUE(roundtrip->is_extern);
    ASSERT_NOT_NULL(roundtrip->ffi_sig);
    ASSERT_STR_EQ(roundtrip->ffi_sig->symbol, "pow");
    ASSERT_NOT_NULL(roundtrip->ffi_sig->dylib);
    ASSERT_STR_EQ(roundtrip->ffi_sig->dylib, "m");
    ASSERT_EQ_UINT(roundtrip->ffi_sig->nparams, 2);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->params[0], XR_FFI_T_F64);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->params[1], XR_FFI_T_F64);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->ret, XR_FFI_T_F64);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_extern_default_library) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    /* @extern fn sqrt(x: float64) -> float64 (no @dylib -> default/process). */
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    proto->is_extern = true;
    XrFFISig *sig = xr_ffi_sig_new("sqrt", NULL, 1);
    ASSERT_NOT_NULL(sig);
    sig->params[0] = XR_FFI_T_F64;
    sig->ret = XR_FFI_T_F64;
    proto->ffi_sig = sig;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_TRUE(roundtrip->is_extern);
    ASSERT_NOT_NULL(roundtrip->ffi_sig);
    ASSERT_STR_EQ(roundtrip->ffi_sig->symbol, "sqrt");
    ASSERT_NULL(roundtrip->ffi_sig->dylib);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->nparams, 1);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_extern_cfn_callback_signature) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    proto->is_extern = true;
    XrFFISig *sig = xr_ffi_sig_new("call_cb", NULL, 1);
    ASSERT_NOT_NULL(sig);
    uint8_t cb_params[] = {XR_FFI_T_I32};
    ASSERT_TRUE(xr_ffi_sig_set_param_callback_codes(sig, 0, cb_params, 1, XR_FFI_T_I32));
    sig->ret = XR_FFI_T_I32;
    proto->ffi_sig = sig;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_TRUE(roundtrip->is_extern);
    ASSERT_NOT_NULL(roundtrip->ffi_sig);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->nparams, 1);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->params[0], XR_FFI_T_PTR);
    ASSERT_NOT_NULL(roundtrip->ffi_sig->param_cbacks);
    ASSERT_NOT_NULL(roundtrip->ffi_sig->param_cbacks[0]);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->param_cbacks[0]->nparams, 1);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->param_cbacks[0]->params[0], XR_FFI_T_I32);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->param_cbacks[0]->ret, XR_FFI_T_I32);
    ASSERT_EQ_UINT(roundtrip->ffi_sig->ret, XR_FFI_T_I32);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

static void run_all_tests(void) {
    RUN_TEST_SUITE("Bytecode I/O");
    RUN_TEST(bytecode_write_emits_current_header_and_roundtrips_u64_instruction);
    RUN_TEST(bytecode_roundtrips_reachable_entry_plan);
    RUN_TEST(bytecode_roundtrips_struct_area_size);
    RUN_TEST(bytecode_roundtrips_exact_string_constant_lengths);
    RUN_TEST(bytecode_reader_assigns_unique_proto_ids);
    RUN_TEST(bytecode_reader_rejects_previous_layout_version);
    RUN_TEST(bytecode_roundtrips_dynamic_json_shape_across_isolates);
    RUN_TEST(bytecode_roundtrips_class_descriptor_constants);
    RUN_TEST(bytecode_roundtrips_enum_type_constants);
    RUN_TEST(bytecode_roundtrips_u16_upvalue_index);
    RUN_TEST(bytecode_roundtrips_declared_shared_count);
    RUN_TEST(bytecode_roundtrips_symbol_index_above_255);
    RUN_TEST(bytecode_roundtrips_extern_ffi_signature);
    RUN_TEST(bytecode_roundtrips_extern_default_library);
    RUN_TEST(bytecode_roundtrips_extern_cfn_callback_signature);
}

TEST_MAIN_BEGIN()
printf("=== xray Bytecode I/O Unit Tests ===\n");
run_all_tests();
TEST_MAIN_END()
