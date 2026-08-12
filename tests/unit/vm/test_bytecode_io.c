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
#include "module/xproto_codec.h"
#include "plan/format/xr_artifact_kind.h"
#include "runtime/xisolate_api.h"
#include "runtime/xexec_state.h"
#include "runtime/class/xclass.h"
#include "runtime/class/xclass_descriptor.h"
#include "runtime/class/xenum.h"
#include "runtime/class/xinstance.h"
#include "runtime/class/xclass_system.h"
#include "runtime/object/xstring.h"
#include "runtime/object/xbigint.h"
#include "runtime/symbol/xsymbol_table.h"
#include "runtime/value/xchunk.h"
#include "runtime/value/xffi_sig.h"
#include "runtime/value/xstruct_layout.h"
#include "../../../stdlib/stdlib_cache.h"
#include "xray_vm.h"

#include <string.h>

static XrVMRuntime *new_test_isolate(void) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    return xray_vm_new_full(&params);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t) p[0] | (uint16_t) ((uint16_t) p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++)
        value |= (uint64_t) p[i] << (i * 8u);
    return value;
}

static void write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t) value;
    p[1] = (uint8_t) (value >> 8);
}

static void write_le32(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++)
        p[i] = (uint8_t) (value >> (i * 8u));
}

static void write_le64(uint8_t *p, uint64_t value) {
    for (uint32_t i = 0; i < 8; i++)
        p[i] = (uint8_t) (value >> (i * 8u));
}

static XrAggregateLayout test_layout(uint8_t kind, uint16_t field_count) {
    XrAggregateLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.kind = kind;
    layout.field_count = field_count;
    return layout;
}

static XrClassDescriptor test_layout_descriptor(const char *name, XrAggregateLayout *layout) {
    XrClassDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.class_name = name;
    desc.descriptor_version = XR_CLASS_DESCRIPTOR_VERSION;
    desc.clinit_proto_index = -1;
    desc.super_global_index = -1;
    desc.struct_layout = layout;
    return desc;
}

static void free_minimal_roundtrip_descriptor(XrClassDescriptor *desc) {
    if (!desc)
        return;
    xr_free((void *) desc->class_name);
    xr_free((void *) desc->super_name);
    xr_free((void *) desc->generic_origin_name);
    xr_free((void *) desc->display_name);
    xr_free(desc);
}

/* Locate the first nested-layout key in the canonical layout table.  Keeping
 * this parser in the test makes corruption checks independent of qsort order. */
static bool find_nested_key_offset(const uint8_t *bytes, size_t size, size_t *out_offset,
                                   uint64_t *out_owner_key) {
    if (!bytes || size < 24)
        return false;
    uint32_t count = read_le32(bytes + 20);
    size_t pos = 24;
    for (uint32_t li = 0; li < count; li++) {
        if (pos > size || size - pos < 33)
            return false;
        uint64_t owner_key = read_le64(bytes + pos + 4);
        pos += 33;
        /* v3 layout metadata carries an optional nominal struct name before
         * the field count. */
        if (pos >= size)
            return false;
        uint8_t has_nominal_name = bytes[pos++];
        if (has_nominal_name == 1) {
            if (pos > size || size - pos < 4)
                return false;
            uint32_t length = read_le32(bytes + pos);
            pos += 4;
            if (pos > size || length > size - pos)
                return false;
            pos += length;
        } else if (has_nominal_name != 0) {
            return false;
        }
        if (pos > size || size - pos < 2)
            return false;
        uint16_t field_count = read_le16(bytes + pos);
        pos += 2;
        for (uint16_t fi = 0; fi < field_count; fi++) {
            if (pos >= size)
                return false;
            uint8_t has_name = bytes[pos++];
            if (has_name == 1) {
                if (pos > size || size - pos < 4)
                    return false;
                uint32_t length = read_le32(bytes + pos);
                pos += 4;
                if (pos > size || length > size - pos)
                    return false;
                pos += length;
            } else if (has_name != 0) {
                return false;
            }
            if (pos > size || size - pos < 23)
                return false;
            if (bytes[pos + 8] == XR_NATIVE_NESTED_AGGREGATE) {
                *out_offset = pos + 14;
                *out_owner_key = owner_key;
                return true;
            }
            pos += 23;
        }
    }
    return false;
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
    proto->call_place_param_bitmap = UINT64_C(0x8000000000000003);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);
    ASSERT_GT(size, 16);
    ASSERT_EQ_UINT(read_le16(bytes + 4), XR_LEGACY_XRC_VERSION);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CODE_COUNT(roundtrip), 1);
    ASSERT_EQ_INT(GET_OPCODE(PROTO_CODE(roundtrip, 0)), OP_RETURN);
    ASSERT_EQ_INT(roundtrip->maxstacksize, 1);
    ASSERT_EQ_UINT(roundtrip->call_place_param_bitmap, UINT64_C(0x8000000000000003));
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
    uint8_t *bytes = xr_bytecode_write(iso, root, 0, &size, NULL);
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
    proto->struct_area_size = 1024u * 1024u;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(iso, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_UINT(roundtrip->struct_area_size, 1024u * 1024u);

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
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 2);
    XrValue value = PROTO_CONSTANT(roundtrip, 0);
    ASSERT_TRUE(XR_IS_STRING(value));
    ASSERT_TRUE(XR_STR_IS_PERMANENT(XR_TO_STRING(value)));
    ASSERT_EQ_UINT(XR_TO_STRING(value)->length, 0);
    ASSERT_STR_EQ(XR_TO_STRING(value)->data, "");
    value = PROTO_CONSTANT(roundtrip, 1);
    ASSERT_TRUE(XR_IS_STRING(value));
    ASSERT_TRUE(XR_STR_IS_PERMANENT(XR_TO_STRING(value)));
    ASSERT_EQ_UINT(XR_TO_STRING(value)->length, sizeof(embedded_nul));
    ASSERT_TRUE(memcmp(XR_TO_STRING(value)->data, embedded_nul, sizeof(embedded_nul)) == 0);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_rune_constants) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, xr_rune(0x1F642)), 0);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 1);
    XrValue value = PROTO_CONSTANT(roundtrip, 0);
    ASSERT_TRUE(XR_IS_RUNE(value));
    ASSERT_EQ_UINT(XR_TO_RUNE(value), 0x1F642);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

/* Regression: BigInt literal constants must survive bytecode serialization.
 * Before the writer/reader learned the BigInt case, every BigInt constant was
 * emitted as null, so embedded-bytecode builds and .xrc artifacts silently lost
 * the value (var b = 123n printed null). Round-trip small, large-magnitude and
 * negative values and require an exact decimal match. */
TEST(bytecode_roundtrips_bigint_constants) {
    static const char *const cases[] = {
        "0",
        "123",
        "-456",
        "12345678901234567890",              /* exceeds int64 range */
        "-99999999999999999999999999999999", /* large negative magnitude */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        XrVMRuntime *writer = new_test_isolate();
        ASSERT_NOT_NULL(writer);
        XrVMRuntime *reader = new_test_isolate();
        ASSERT_NOT_NULL(reader);

        XrProto *proto = make_minimal_proto();
        ASSERT_NOT_NULL(proto);

        /* Build the constant exactly as the emitter does: a fixed-heap BigInt
         * whose class pointer is the isolate's registered BigInt class, so
         * XR_IS_BIGINT recognizes it on the writer side. */
        XrBigInt *bi =
            xr_bigint_from_string_on_fixed_heap(xr_isolate_get_fixed_heap(writer), cases[i]);
        ASSERT_NOT_NULL(bi);
        XrayCoreClasses *writer_core = xr_isolate_get_core_classes(writer);
        ASSERT_NOT_NULL(writer_core);
        bi->klass = writer_core->bigintClass;
        XrValue original = XR_FROM_PTR(bi);
        ASSERT_TRUE(XR_IS_BIGINT(original));
        ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, original), 0);

        size_t size = 0;
        uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
        ASSERT_NOT_NULL(bytes);

        XrBcError error = XR_BC_OK;
        XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
        ASSERT_NOT_NULL(roundtrip);
        ASSERT_EQ_INT(error, XR_BC_OK);
        ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 1);

        XrValue value = PROTO_CONSTANT(roundtrip, 0);
        ASSERT_TRUE(XR_IS_BIGINT(value));
        char *decoded = xr_bigint_to_string((XrBigInt *) XR_TO_PTR(value));
        ASSERT_NOT_NULL(decoded);
        ASSERT_STR_EQ(decoded, cases[i]);
        xr_free(decoded);

        xr_vm_proto_free(roundtrip);
        xr_free(bytes);
        xr_vm_proto_free(proto);
        xray_vm_delete(reader);
        xray_vm_delete(writer);
    }
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
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);
    ASSERT_GT(size, 16);

    uint16_t previous_version = (uint16_t) (XR_LEGACY_XRC_VERSION - 1);
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

TEST(bytecode_roundtrips_exact_structural_shape_across_isolates) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    const char *names[] = {"host", "port"};
    const uint64_t stable_type_keys[] = {UINT64_C(0x1111), UINT64_C(0x2222)};
    const uint8_t shape_flags[] = {XR_OBJECT_SHAPE_FIELD_READONLY, 0};
    XrClass *shape = xr_class_build_struct_object_chain(writer, names, NULL, 2, NULL, NULL,
                                                        stable_type_keys, shape_flags);
    ASSERT_NOT_NULL(shape);
    const XrtObjectShapeField manifest[] = {
        {"host", stable_type_keys[0], 0, 0, shape_flags[0], 0},
        {"port", stable_type_keys[1], 0, 1, shape_flags[1], 0},
    };
    ASSERT_EQ_UINT(xr_class_stable_shape_key(shape),
                   xr_object_shape_stable_key(XR_OBJECT_DOMAIN_STRUCT, manifest, 2));

    int kidx = xr_valuearray_add(&proto->constants, xr_int((int64_t) (intptr_t) shape));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABC(OP_NEWOBJECT, 0, kidx, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
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
    ASSERT_EQ_UINT(roundtrip_shape->builtin_kind, XR_BK_STRUCT_OBJECT);
    ASSERT_TRUE((roundtrip_shape->flags & XR_CLASS_DYNAMIC_LAYOUT) != 0);
    ASSERT_TRUE((roundtrip_shape->flags & XR_CLASS_DYNAMIC_SEALED) != 0);
    ASSERT_EQ_UINT(roundtrip_shape->field_count, 2);
    ASSERT_EQ_UINT(xr_class_object_domain(roundtrip_shape), XR_OBJECT_DOMAIN_STRUCT);
    ASSERT_EQ_UINT(xr_class_stable_shape_key(roundtrip_shape), xr_class_stable_shape_key(shape));
    ASSERT_EQ_UINT(roundtrip_shape->fields[0].stable_type_key, stable_type_keys[0]);
    ASSERT_EQ_UINT(roundtrip_shape->fields[0].shape_flags, shape_flags[0]);
    ASSERT_STR_EQ(roundtrip_shape->fields[0].name, "host");
    ASSERT_STR_EQ(roundtrip_shape->fields[1].name, "port");

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_typed_object_decode_shape) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    const char *nested_names[] = {"city", "zip"};
    const uint8_t nested_kinds[] = {XR_JSON_VALUE_STRING, XR_JSON_VALUE_INT};
    const uint64_t nested_type_keys[] = {UINT64_C(0x3101), UINT64_C(0x3102)};
    const uint8_t nested_flags[] = {0, XR_OBJECT_SHAPE_FIELD_READONLY};
    XrClass *nested_shape = xr_class_build_struct_object_chain(
        writer, nested_names, nested_kinds, 2, NULL, NULL, nested_type_keys, nested_flags);
    ASSERT_NOT_NULL(nested_shape);

    const char *names[] = {"name", "address", "items"};
    const uint8_t kinds[] = {XR_JSON_VALUE_STRING, XR_JSON_VALUE_STRUCT_OBJECT,
                             XR_JSON_VALUE_ARRAY};
    XrClass *nested_shapes[] = {NULL, nested_shape, NULL};
    const XrJsonDecodeSchema array_item_schema = {
        .value_kind = XR_JSON_VALUE_JSON,
        .storage_type = XR_ELEM_ANY,
    };
    const XrJsonDecodeSchema schemas[] = {
        {.value_kind = XR_JSON_VALUE_STRING},
        {.value_kind = XR_JSON_VALUE_STRUCT_OBJECT, .target_descriptor = nested_shape},
        {.value_kind = XR_JSON_VALUE_ARRAY,
         .storage_type = XR_ELEM_ANY,
         .child = &array_item_schema},
    };
    const uint64_t stable_type_keys[] = {UINT64_C(0x4101), UINT64_C(0x4102), UINT64_C(0x4103)};
    const uint8_t shape_flags[] = {XR_OBJECT_SHAPE_FIELD_READONLY, 0, 0};
    XrClass *shape = xr_class_build_struct_object_chain(writer, names, kinds, 3, nested_shapes,
                                                        schemas, stable_type_keys, shape_flags);
    ASSERT_NOT_NULL(shape);
    const XrtObjectShapeField manifest[] = {
        {"name", stable_type_keys[0], 0, 0, shape_flags[0], 0},
        {"address", stable_type_keys[1], 0, 1, shape_flags[1], 0},
        {"items", stable_type_keys[2], 0, 2, shape_flags[2], 0},
    };
    ASSERT_EQ_UINT(xr_class_stable_shape_key(shape),
                   xr_object_shape_stable_key(XR_OBJECT_DOMAIN_STRUCT, manifest, 3));

    int kidx = xr_valuearray_add(&proto->constants, xr_int((int64_t) (intptr_t) shape));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 2;
    xr_vm_proto_write(proto, CREATE_ABC(OP_JSON_DECODE, 0, 1, kidx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    XrClass *roundtrip_shape = (XrClass *) (intptr_t) XR_TO_INT(PROTO_CONSTANT(roundtrip, 0));
    ASSERT_NOT_NULL(roundtrip_shape);
    ASSERT_EQ_UINT(roundtrip_shape->builtin_kind, XR_BK_STRUCT_OBJECT);
    ASSERT_TRUE((roundtrip_shape->flags & XR_CLASS_DYNAMIC_SEALED) != 0);
    ASSERT_EQ_UINT(roundtrip_shape->field_count, 3);
    ASSERT_EQ_UINT(xr_class_object_domain(roundtrip_shape), XR_OBJECT_DOMAIN_STRUCT);
    ASSERT_EQ_UINT(xr_class_stable_shape_key(roundtrip_shape), xr_class_stable_shape_key(shape));
    ASSERT_EQ_UINT(roundtrip_shape->fields[2].stable_type_key, stable_type_keys[2]);
    ASSERT_EQ_UINT(roundtrip_shape->fields[2].shape_flags, shape_flags[2]);
    ASSERT_EQ_UINT(roundtrip_shape->fields[0].json_value_kind, XR_JSON_VALUE_STRING);
    ASSERT_EQ_UINT(roundtrip_shape->fields[1].json_value_kind, XR_JSON_VALUE_STRUCT_OBJECT);
    ASSERT_EQ_UINT(roundtrip_shape->fields[2].json_decode_schema.value_kind, XR_JSON_VALUE_ARRAY);
    ASSERT_NOT_NULL(roundtrip_shape->fields[2].json_decode_schema.child);
    ASSERT_EQ_UINT(roundtrip_shape->fields[2].json_decode_schema.child->value_kind,
                   XR_JSON_VALUE_JSON);
    XrClass *roundtrip_nested = roundtrip_shape->fields[1].json_struct_object_class;
    ASSERT_NOT_NULL(roundtrip_nested);
    ASSERT_EQ_UINT(roundtrip_nested->builtin_kind, XR_BK_STRUCT_OBJECT);
    ASSERT_EQ_UINT(roundtrip_nested->field_count, 2);
    ASSERT_STR_EQ(roundtrip_nested->fields[0].name, "city");
    ASSERT_EQ_UINT(roundtrip_nested->fields[0].json_value_kind, XR_JSON_VALUE_STRING);
    ASSERT_STR_EQ(roundtrip_nested->fields[1].name, "zip");
    ASSERT_EQ_UINT(roundtrip_nested->fields[1].json_value_kind, XR_JSON_VALUE_INT);
    ASSERT_EQ_UINT(roundtrip_shape->fields[2].json_value_kind, XR_JSON_VALUE_ARRAY);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_typed_json_root_schema) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    char *enum_names[] = {"Text", "Binary"};
    XrEnumType *enum_type = xr_enum_type_new(writer, "test.bytecode", "WireKind", enum_names, 2);
    ASSERT_NOT_NULL(enum_type);
    const XrJsonDecodeSchema enum_schema = {
        .value_kind = XR_JSON_VALUE_ENUM,
        .storage_type = XR_ELEM_ANY,
        .target_descriptor = enum_type,
    };
    const XrJsonDecodeSchema array_schema = {
        .value_kind = XR_JSON_VALUE_ARRAY,
        .storage_type = XR_ELEM_ANY,
        .child = &enum_schema,
    };
    const XrJsonDecodeSchema map_schema = {
        .value_kind = XR_JSON_VALUE_MAP,
        .storage_type = XR_ELEM_ANY,
        .child = &array_schema,
    };
    const char *names[] = {"\x1fjson_decode_root"};
    const uint8_t kinds[] = {XR_JSON_VALUE_MAP};
    const uint64_t stable_type_keys[] = {UINT64_C(0x5201)};
    const uint8_t shape_flags[] = {0};
    XrClass *shape = xr_class_build_struct_object_chain(writer, names, kinds, 1, NULL, &map_schema,
                                                        stable_type_keys, shape_flags);
    ASSERT_NOT_NULL(shape);
    shape->flags |= XR_CLASS_JSON_DECODE_ROOT;

    int kidx = xr_valuearray_add(&proto->constants, xr_int((int64_t) (intptr_t) shape));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 2;
    xr_vm_proto_write(proto, CREATE_ABC(OP_JSON_DECODE, 0, 1, kidx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);

    XrBcError error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    XrClass *roundtrip_shape = (XrClass *) (intptr_t) XR_TO_INT(PROTO_CONSTANT(roundtrip, 0));
    ASSERT_NOT_NULL(roundtrip_shape);
    ASSERT_TRUE((roundtrip_shape->flags & XR_CLASS_JSON_DECODE_ROOT) != 0);
    ASSERT_EQ_UINT(roundtrip_shape->field_count, 1);
    const XrJsonDecodeSchema *roundtrip_schema = &roundtrip_shape->fields[0].json_decode_schema;
    ASSERT_EQ_UINT(roundtrip_schema->value_kind, XR_JSON_VALUE_MAP);
    ASSERT_NOT_NULL(roundtrip_schema->child);
    ASSERT_EQ_UINT(roundtrip_schema->child->value_kind, XR_JSON_VALUE_ARRAY);
    ASSERT_EQ_UINT(roundtrip_schema->child->storage_type, XR_ELEM_ANY);
    ASSERT_NOT_NULL(roundtrip_schema->child->child);
    ASSERT_EQ_UINT(roundtrip_schema->child->child->value_kind, XR_JSON_VALUE_ENUM);
    XrEnumType *roundtrip_enum = (XrEnumType *) roundtrip_schema->child->child->target_descriptor;
    ASSERT_NOT_NULL(roundtrip_enum);
    ASSERT_STR_EQ(roundtrip_enum->name, "WireKind");
    ASSERT_EQ_UINT(roundtrip_enum->member_count, 2);
    ASSERT_STR_EQ(xr_enum_type_member_name(roundtrip_enum, 0), "Text");
    ASSERT_STR_EQ(xr_enum_type_member_name(roundtrip_enum, 1), "Binary");
    ASSERT_TRUE(xr_enum_type_same_nominal(enum_type, roundtrip_enum));

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
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
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

TEST(bytecode_roundtrips_canonical_layout_matrix_deterministically) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    ASSERT_NOT_NULL(target);

    const char *child_names[] = {"tag", "payload"};
    XrAggregateLayout child = test_layout(XR_AGG_LAYOUT_PACKED_STRUCT, 2);
    child.field_names = child_names;
    child.fields[0].native_type = XR_NATIVE_U8;
    child.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&child, target));

    const char *outer_names[] = {"header", "words", "count", "tail"};
    XrAggregateLayout outer = test_layout(XR_AGG_LAYOUT_STRUCT, 4);
    outer.nominal_name = "OuterValue";
    outer.field_names = outer_names;
    outer.fields[0].native_type = XR_NATIVE_NESTED_AGGREGATE;
    outer.fields[0].sub_layout = &child;
    outer.fields[1].native_type = XR_NATIVE_ARRAY;
    outer.fields[1].elem_native_type = XR_NATIVE_U16;
    outer.fields[1].elem_count = 3;
    outer.fields[2].native_type = XR_NATIVE_USIZE;
    outer.fields[3].native_type = XR_NATIVE_ARRAY;
    outer.fields[3].elem_native_type = XR_NATIVE_U8;
    outer.fields[3].elem_count = 5;
    ASSERT_TRUE(xr_aggregate_layout_compute(&outer, target));

    /* A separately allocated but semantically equal graph must serialize once
     * and resolve to the same canonical runtime pointer. */
    XrAggregateLayout child_copy = test_layout(XR_AGG_LAYOUT_PACKED_STRUCT, 2);
    child_copy.field_names = child_names;
    child_copy.fields[0].native_type = XR_NATIVE_U8;
    child_copy.fields[1].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&child_copy, target));
    XrAggregateLayout outer_copy = test_layout(XR_AGG_LAYOUT_STRUCT, 4);
    outer_copy.nominal_name = "OuterValue";
    outer_copy.field_names = outer_names;
    outer_copy.fields[0].native_type = XR_NATIVE_NESTED_AGGREGATE;
    outer_copy.fields[0].sub_layout = &child_copy;
    outer_copy.fields[1].native_type = XR_NATIVE_ARRAY;
    outer_copy.fields[1].elem_native_type = XR_NATIVE_U16;
    outer_copy.fields[1].elem_count = 3;
    outer_copy.fields[2].native_type = XR_NATIVE_USIZE;
    outer_copy.fields[3].native_type = XR_NATIVE_ARRAY;
    outer_copy.fields[3].elem_native_type = XR_NATIVE_U8;
    outer_copy.fields[3].elem_count = 5;
    ASSERT_TRUE(xr_aggregate_layout_compute(&outer_copy, target));
    ASSERT_TRUE(xr_aggregate_layout_semantically_equal(&outer, &outer_copy));

    /* Equal bytes do not imply equal language types.  A different nominal
     * name must survive bytecode roundtrip as a distinct layout identity. */
    XrAggregateLayout outer_distinct = outer;
    outer_distinct.layout_id = 0;
    outer_distinct.nominal_name = "OtherValue";
    ASSERT_FALSE(xr_aggregate_layout_semantically_equal(&outer, &outer_distinct));

    const char *union_names[] = {"bits", "number"};
    XrAggregateLayout variant = test_layout(XR_AGG_LAYOUT_UNION, 2);
    variant.field_names = union_names;
    variant.explicit_align = 16;
    variant.fields[0].native_type = XR_NATIVE_U64;
    variant.fields[1].native_type = XR_NATIVE_F64;
    ASSERT_TRUE(xr_aggregate_layout_compute(&variant, target));

    XrClassDescriptor desc_outer = test_layout_descriptor("Outer", &outer);
    XrClassDescriptor desc_copy = test_layout_descriptor("OuterAlias", &outer_copy);
    XrClassDescriptor desc_union = test_layout_descriptor("Variant", &variant);
    XrClassDescriptor desc_distinct = test_layout_descriptor("Other", &outer_distinct);
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc_outer)), 0);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc_copy)), 1);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc_union)), 2);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc_distinct)), 3);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 1), 1);
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 2), 1);
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 3), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size_a = 0;
    size_t size_b = 0;
    XrBcError write_error = XR_BC_OK;
    uint8_t *bytes_a = xr_bytecode_write(writer, proto, 0, &size_a, &write_error);
    ASSERT_NOT_NULL(bytes_a);
    ASSERT_EQ_INT(write_error, XR_BC_OK);
    uint8_t *bytes_b = xr_bytecode_write(writer, proto, 0, &size_b, &write_error);
    ASSERT_NOT_NULL(bytes_b);
    ASSERT_EQ_UINT(size_a, size_b);
    ASSERT_MEM_EQ(bytes_a, bytes_b, size_a);
    ASSERT_EQ_UINT(read_le32(bytes_a + 20), 4);

    XrBcError read_error = XR_BC_OK;
    XrProto *roundtrip = xr_bytecode_read(reader, bytes_a, size_a, &read_error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(read_error, XR_BC_OK);
    XrClassDescriptor *read_outer = XR_TO_PTR(PROTO_CONSTANT(roundtrip, 0));
    XrClassDescriptor *read_copy = XR_TO_PTR(PROTO_CONSTANT(roundtrip, 1));
    XrClassDescriptor *read_union = XR_TO_PTR(PROTO_CONSTANT(roundtrip, 2));
    XrClassDescriptor *read_distinct = XR_TO_PTR(PROTO_CONSTANT(roundtrip, 3));
    ASSERT_NOT_NULL(read_outer);
    ASSERT_NOT_NULL(read_copy);
    ASSERT_NOT_NULL(read_union);
    ASSERT_NOT_NULL(read_distinct);
    ASSERT_EQ_PTR(read_outer->struct_layout, read_copy->struct_layout);
    ASSERT_TRUE(read_outer->struct_layout != read_distinct->struct_layout);
    ASSERT_STR_EQ(read_outer->struct_layout->nominal_name, "OuterValue");
    ASSERT_STR_EQ(read_distinct->struct_layout->nominal_name, "OtherValue");
    ASSERT_EQ_UINT(read_outer->struct_layout->target_abi_hash, target->stable_hash);
    ASSERT_EQ_UINT(read_outer->struct_layout->kind, XR_AGG_LAYOUT_STRUCT);
    ASSERT_EQ_UINT(read_outer->struct_layout->field_count, 4);
    ASSERT_STR_EQ(read_outer->struct_layout->field_names[0], "header");
    ASSERT_EQ_UINT(read_outer->struct_layout->fields[1].native_type, XR_NATIVE_ARRAY);
    ASSERT_EQ_UINT(read_outer->struct_layout->fields[1].elem_count, 3);
    ASSERT_EQ_UINT(read_outer->struct_layout->fields[3].elem_count, 5);
    ASSERT_NOT_NULL(read_outer->struct_layout->fields[0].sub_layout);
    ASSERT_EQ_UINT(read_outer->struct_layout->fields[0].sub_layout->kind,
                   XR_AGG_LAYOUT_PACKED_STRUCT);
    ASSERT_EQ_UINT(read_outer->struct_layout->fields[0].sub_layout->alignment, 1);
    ASSERT_EQ_UINT(read_union->struct_layout->kind, XR_AGG_LAYOUT_UNION);
    ASSERT_EQ_UINT(read_union->struct_layout->alignment, 16);
    ASSERT_EQ_UINT(read_union->struct_layout->fields[0].offset, 0);
    ASSERT_EQ_UINT(read_union->struct_layout->fields[1].offset, 0);
    ASSERT_TRUE(read_outer->struct_layout->layout_id != 0);

    free_minimal_roundtrip_descriptor(read_outer);
    free_minimal_roundtrip_descriptor(read_copy);
    free_minimal_roundtrip_descriptor(read_union);
    free_minimal_roundtrip_descriptor(read_distinct);
    xr_vm_proto_free(roundtrip);
    xr_free(bytes_b);
    xr_free(bytes_a);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_layout_reader_rejects_abi_offset_count_cycle_and_truncation_corruption) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    ASSERT_NOT_NULL(target);

    XrAggregateLayout scalar = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    scalar.fields[0].native_type = XR_NATIVE_I32;
    ASSERT_TRUE(xr_aggregate_layout_compute(&scalar, target));
    XrClassDescriptor desc = test_layout_descriptor("Scalar", &scalar);
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc)), 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);
    ASSERT_GT(size, 84);
    ASSERT_EQ_UINT(read_le32(bytes + 20), 1);

    XrBcError error = XR_BC_OK;
    bytes[36] ^= 1u;
    ASSERT_NULL(xr_bytecode_read(iso, bytes, size, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_TARGET_ABI);
    bytes[36] ^= 1u;

    write_le16(bytes + 58, XR_MAX_AGG_FIELDS + 1);
    ASSERT_NULL(xr_bytecode_read(iso, bytes, size, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_CORRUPT);
    write_le16(bytes + 58, 1);

    write_le32(bytes + 61, 1);
    ASSERT_NULL(xr_bytecode_read(iso, bytes, size, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_CORRUPT);
    write_le32(bytes + 61, 0);

    ASSERT_NULL(xr_bytecode_read(iso, bytes, 60, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_TRUNCATED);
    ASSERT_NULL(xr_bytecode_read(iso, bytes, 3, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_TRUNCATED);
    uint8_t *with_trailing_byte = xr_malloc(size + 1);
    ASSERT_NOT_NULL(with_trailing_byte);
    memcpy(with_trailing_byte, bytes, size);
    with_trailing_byte[size] = 0;
    ASSERT_NULL(xr_bytecode_read(iso, with_trailing_byte, size + 1, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_CORRUPT);
    xr_free(with_trailing_byte);
    ASSERT_STR_EQ(xr_bytecode_error_string(XR_BC_ERR_TARGET_ABI),
                  "bytecode aggregate layout target ABI mismatch");
    ASSERT_STR_EQ(xr_bytecode_error_string(XR_BC_ERR_CORRUPT), "corrupt bytecode metadata");

    xr_free(bytes);
    xr_vm_proto_free(proto);

    XrAggregateLayout child = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    child.fields[0].native_type = XR_NATIVE_U8;
    ASSERT_TRUE(xr_aggregate_layout_compute(&child, target));
    XrAggregateLayout outer = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    outer.fields[0].native_type = XR_NATIVE_NESTED_AGGREGATE;
    outer.fields[0].sub_layout = &child;
    ASSERT_TRUE(xr_aggregate_layout_compute(&outer, target));
    XrClassDescriptor nested_desc = test_layout_descriptor("Nested", &outer);
    proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&nested_desc)), 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);
    bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
    ASSERT_NOT_NULL(bytes);
    size_t nested_key_offset = 0;
    uint64_t owner_key = 0;
    ASSERT_TRUE(find_nested_key_offset(bytes, size, &nested_key_offset, &owner_key));
    write_le64(bytes + nested_key_offset, owner_key);
    ASSERT_NULL(xr_bytecode_read(iso, bytes, size, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_CORRUPT);

    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(bytecode_layout_writer_rejects_target_mismatch_and_excessive_depth) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    ASSERT_NOT_NULL(target);

    XrAggregateLayout wrong_target = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    wrong_target.fields[0].native_type = XR_NATIVE_POINTER;
    ASSERT_TRUE(xr_aggregate_layout_compute(&wrong_target, target));
    wrong_target.target_abi_hash ^= UINT64_C(1);
    XrClassDescriptor desc = test_layout_descriptor("WrongTarget", &wrong_target);
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc)), 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);
    size_t size = 0;
    XrBcError error = XR_BC_OK;
    ASSERT_NULL(xr_bytecode_write(iso, proto, 0, &size, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_TARGET_ABI);
    xr_vm_proto_free(proto);

    XrAggregateLayout chain[18];
    for (uint32_t i = 0; i < 18; i++)
        chain[i] = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    chain[17].fields[0].native_type = XR_NATIVE_U8;
    ASSERT_TRUE(xr_aggregate_layout_compute(&chain[17], target));
    for (int i = 16; i >= 0; i--) {
        chain[i].fields[0].native_type = XR_NATIVE_NESTED_AGGREGATE;
        chain[i].fields[0].sub_layout = &chain[i + 1];
        ASSERT_TRUE(xr_aggregate_layout_compute(&chain[i], target));
    }
    desc = test_layout_descriptor("TooDeep", &chain[0]);
    proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(&desc)), 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    xr_vm_proto_write(proto, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, 0, 0), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);
    ASSERT_NULL(xr_bytecode_write(iso, proto, 0, &size, &error));
    ASSERT_EQ_INT(error, XR_BC_ERR_METADATA);

    xr_vm_proto_free(proto);
    xray_vm_delete(iso);
}

TEST(vm_struct_layout_class_resolves_semantically_equal_descriptor_copy) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);
    XrVMState *vm = xr_isolate_get_vm_state(iso);
    const XrTargetDataLayout *target = xr_target_data_layout_host();
    ASSERT_NOT_NULL(vm);
    ASSERT_NOT_NULL(target);

    const char *field_names[] = {"value"};
    XrAggregateLayout declared = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    declared.nominal_name = "NestedValue";
    declared.field_names = field_names;
    declared.fields[0].native_type = XR_NATIVE_I32;
    ASSERT_TRUE(xr_aggregate_layout_compute(&declared, target));

    XrAggregateLayout embedded_copy = test_layout(XR_AGG_LAYOUT_STRUCT, 1);
    embedded_copy.nominal_name = "NestedValue";
    embedded_copy.field_names = field_names;
    embedded_copy.fields[0].native_type = XR_NATIVE_I32;
    ASSERT_TRUE(xr_aggregate_layout_compute(&embedded_copy, target));
    ASSERT_TRUE(xr_aggregate_layout_semantically_equal(&declared, &embedded_copy));

    XrClass identity;
    memset(&identity, 0, sizeof(identity));
    uint16_t declared_id = xr_vm_struct_layout_register(vm, &declared);
    uint16_t copy_id = xr_vm_struct_layout_register(vm, &embedded_copy);
    ASSERT_TRUE(declared_id != 0);
    ASSERT_TRUE(copy_id != 0);
    ASSERT_TRUE(declared_id != copy_id);
    ASSERT_TRUE(xr_vm_struct_layout_bind_class(vm, &declared, &identity));
    ASSERT_EQ_PTR(xr_vm_struct_layout_class(vm, copy_id), &identity);

    xray_vm_delete(iso);
}

TEST(bytecode_roundtrips_enum_type_constants) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);

    char *members[] = {"Ok", "Err"};
    XrEnumType *enum_type = xr_enum_type_new(writer, "test.bytecode", "BytecodeResult", members, 2);
    ASSERT_NOT_NULL(enum_type);
    int payload_counts[] = {0, 1};
    ASSERT_TRUE(xr_enum_type_set_adt_payloads(enum_type, payload_counts, 2));
    const char *payload_names[] = {"reason"};
    uint8_t payload_type_ids[] = {XR_TID_STRING};
    ASSERT_TRUE(xr_enum_layout_set_variant_payload_metadata(enum_type->layout, 1, payload_names,
                                                            payload_type_ids, 1));
    enum_type->derive_flags = XR_DERIVE_INSPECT | XR_DERIVE_EQ;

    int kidx = xr_valuearray_add(&proto->constants, XR_FROM_PTR(enum_type));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABx(OP_LOADK, 0, kidx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(writer, proto, 0, &size, NULL);
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
    const XrEnumVariantLayout *err_variant = xr_enum_layout_variant(roundtrip_enum->layout, 1);
    ASSERT_NOT_NULL(err_variant);
    ASSERT_NOT_NULL(err_variant->payload_names);
    ASSERT_NOT_NULL(err_variant->payload_type_ids);
    ASSERT_STR_EQ(err_variant->payload_names[0], "reason");
    ASSERT_EQ_UINT(err_variant->payload_type_ids[0], XR_TID_STRING);
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

TEST(bytecode_preserves_native_stdlib_enum_nominal_identity_across_modules) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);
    XrVMRuntime *reader = new_test_isolate();
    ASSERT_NOT_NULL(reader);

    XrEnumType *writer_canonical = xr_stdlib_enum_type_get(writer, "net", "NetError");
    XrEnumType *reader_enum = xr_stdlib_enum_type_get(reader, "net", "NetError");
    ASSERT_NOT_NULL(writer_canonical);
    ASSERT_NOT_NULL(reader_enum);

    /* The source compiler creates a distinct enum object for the declaration.
     * Stdlib serialization must attach the explicit module identity rather
     * than depending on pointer identity with the native cache. */
    char *members[10];
    ASSERT_EQ_UINT(writer_canonical->member_count, 10);
    for (uint32_t i = 0; i < writer_canonical->member_count; i++)
        members[i] = (char *) xr_enum_type_member_name(writer_canonical, i);
    XrEnumType *writer_enum = xr_enum_type_new(writer, "net", "NetError", members, 10);
    ASSERT_NOT_NULL(writer_enum);
    ASSERT_TRUE(writer_enum != writer_canonical);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    int kidx = xr_valuearray_add(&proto->constants, XR_FROM_PTR(writer_enum));
    ASSERT_EQ_INT(kidx, 0);
    proto->code.count = 0;
    proto->lineinfo.count = 0;
    proto->maxstacksize = 1;
    xr_vm_proto_write(proto, CREATE_ABx(OP_LOADK, 0, kidx), 1);
    xr_vm_proto_write(proto, CREATE_ABC(OP_RETURN, 0, 1, 0), 1);

    size_t size = 0;
    XrBcError error = XR_BC_OK;
    uint8_t *bytes = xr_bytecode_write_stdlib(writer, "net", proto, 0, &size, &error);
    ASSERT_NOT_NULL(bytes);
    ASSERT_EQ_INT(error, XR_BC_OK);

    XrProto *roundtrip = xr_bytecode_read(reader, bytes, size, &error);
    ASSERT_NOT_NULL(roundtrip);
    ASSERT_EQ_INT(error, XR_BC_OK);
    ASSERT_EQ_INT(PROTO_CONST_COUNT(roundtrip), 1);
    ASSERT_TRUE(XR_IS_ENUM_TYPE(PROTO_CONSTANT(roundtrip, 0)));
    ASSERT_TRUE(XR_TO_ENUM_TYPE(PROTO_CONSTANT(roundtrip, 0)) == reader_enum);

    xr_vm_proto_free(roundtrip);
    xr_free(bytes);
    xr_vm_proto_free(proto);
    xray_vm_delete(reader);
    xray_vm_delete(writer);
}

TEST(bytecode_rejects_mismatched_native_stdlib_enum_shape) {
    XrVMRuntime *writer = new_test_isolate();
    ASSERT_NOT_NULL(writer);

    char *members[] = {"Closed"};
    XrEnumType *wrong = xr_enum_type_new(writer, "net", "NetError", members, 1);
    ASSERT_NOT_NULL(wrong);
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_valuearray_add(&proto->constants, XR_FROM_PTR(wrong)), 0);

    size_t size = 123;
    XrBcError error = XR_BC_OK;
    ASSERT_NULL(xr_bytecode_write_stdlib(writer, "net", proto, 0, &size, &error));
    ASSERT_EQ_UINT(size, 0);
    ASSERT_EQ_INT(error, XR_BC_ERR_METADATA);

    xr_vm_proto_free(proto);
    xray_vm_delete(writer);
}

TEST(bytecode_roundtrips_u16_upvalue_index) {
    XrVMRuntime *iso = new_test_isolate();
    ASSERT_NOT_NULL(iso);

    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    ASSERT_EQ_INT(xr_vm_proto_add_upvalue(proto, 300, 0, 1, 0, UPVAL_SRC_REG,
                                          XR_TRANSFER_EXPLICIT_COPY, NULL),
                  0);

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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
    ASSERT_EQ_UINT(info.capture_action, XR_TRANSFER_EXPLICIT_COPY);

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
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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

    /* Manifest-mapped extern "C" function pow(base: f64, exp: f64) -> f64.
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
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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

    /* extern "C" fn sqrt(x: f64) -> f64 (default/process lookup). */
    XrProto *proto = make_minimal_proto();
    ASSERT_NOT_NULL(proto);
    proto->is_extern = true;
    XrFFISig *sig = xr_ffi_sig_new("sqrt", NULL, 1);
    ASSERT_NOT_NULL(sig);
    sig->params[0] = XR_FFI_T_F64;
    sig->ret = XR_FFI_T_F64;
    proto->ffi_sig = sig;

    size_t size = 0;
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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
    uint8_t *bytes = xr_bytecode_write(iso, proto, 0, &size, NULL);
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
    RUN_TEST(bytecode_roundtrips_rune_constants);
    RUN_TEST(bytecode_roundtrips_bigint_constants);
    RUN_TEST(bytecode_reader_assigns_unique_proto_ids);
    RUN_TEST(bytecode_reader_rejects_previous_layout_version);
    RUN_TEST(bytecode_roundtrips_exact_structural_shape_across_isolates);
    RUN_TEST(bytecode_roundtrips_typed_object_decode_shape);
    RUN_TEST(bytecode_roundtrips_typed_json_root_schema);
    RUN_TEST(bytecode_roundtrips_class_descriptor_constants);
    RUN_TEST(bytecode_roundtrips_canonical_layout_matrix_deterministically);
    RUN_TEST(bytecode_layout_reader_rejects_abi_offset_count_cycle_and_truncation_corruption);
    RUN_TEST(bytecode_layout_writer_rejects_target_mismatch_and_excessive_depth);
    RUN_TEST(vm_struct_layout_class_resolves_semantically_equal_descriptor_copy);
    RUN_TEST(bytecode_roundtrips_enum_type_constants);
    RUN_TEST(bytecode_preserves_native_stdlib_enum_nominal_identity_across_modules);
    RUN_TEST(bytecode_rejects_mismatched_native_stdlib_enum_shape);
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
