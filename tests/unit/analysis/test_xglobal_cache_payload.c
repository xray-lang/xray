/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xglobal_cache_payload.c - Unit tests for evidence cache payload parsing
 */

#include "../test_framework.h"
#include "../../../src/analysis/xglobal_summary.h"
#include "../../../src/base/xmalloc.h"
#include <string.h>

static void add_sample_module_summary(XgGlobalEvidence *ev) {
    XgModuleSummary module = {0};
    module.module_id = 7;
    module.name_id = xg_name_id("sample");
    module.canonical_hash = UINT64_C(0xabc7);
    module.source_hash = UINT64_C(0xdef7);
    module.kind = 1;
    module.flags = XG_MODULE_EMBEDDED_SOURCE;
    ASSERT_NOT_NULL(xg_global_evidence_add_module(ev, &module));
}

static void add_sample_body_summary(XgGlobalEvidence *ev) {
    XgDeclSummary decl = {0};
    XgBodySummary body = {0};
    XgCallsiteSummary call = {0};
    XgInterfaceObjectUseSummary iface_use = {0};
    XgLinkDependencySummary link = {0};
    XgGenericInstSummary inst = {0};

    decl.decl_id = 1;
    decl.module_id = 7;
    decl.kind = XG_DECL_FUNC;
    decl.name_id = xg_name_id("entry");
    decl.signature_key = 101;
    decl.source_span_id = 3;
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(ev, &decl));

    body.func_id = 11;
    body.module_id = 7;
    body.owner_decl_id = decl.decl_id;
    body.name_id = decl.name_id;
    body.signature_key = decl.signature_key;
    body.source_span_id = 3;
    body.kind = XG_BODY_FUNCTION;
    body.body_hash = UINT64_C(0x123456789abcdef0);
    body.effect_bits = XG_BODY_MAY_CALL;
    body.callsite_start = 1;
    body.callsite_count = 1;
    ASSERT_NOT_NULL(xg_global_evidence_add_body(ev, &body));

    call.callsite_id = 1;
    call.owner_func_id = body.func_id;
    call.source_span_id = 4;
    call.body_ordinal = 1;
    call.kind = XG_CALL_DIRECT_FUNC;
    call.static_target_func_id = body.func_id;
    call.method_name_id = xg_name_id("entry");
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(ev, &call));

    iface_use.use_id = 4;
    iface_use.interface_id = 41;
    iface_use.owner_func_id = body.func_id;
    iface_use.source_span_id = 4;
    iface_use.body_ordinal = 2;
    iface_use.type_key = 930;
    iface_use.reason = XG_INTERFACE_OBJECT_USE_VALUE | XG_INTERFACE_OBJECT_USE_PARAM;
    iface_use.flags = 0x8;
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_object_use(ev, &iface_use));

    link.link_id = 2;
    link.module_id = 7;
    link.decl_id = decl.decl_id;
    link.source_span_id = 5;
    link.name_id = xg_name_id("mem.copy");
    link.kind = XG_LINK_DEP_STDLIB_SYMBOL;
    link.flags = 0x2;
    snprintf(link.name, sizeof(link.name), "%s", "mem.copy");
    ASSERT_NOT_NULL(xg_global_evidence_add_link_dependency(ev, &link));

    inst.generic_inst_id = 3;
    inst.module_id = 7;
    inst.origin_decl_id = decl.decl_id;
    inst.origin_func_id = body.func_id;
    inst.specialized_func_id = 31;
    inst.root_callsite_id = call.callsite_id;
    inst.name_id = xg_name_id("box");
    inst.type_key = 400;
    inst.type_arg_key_start = 401;
    inst.type_arg_count = 1;
    inst.source_span_id = 6;
    inst.kind = XG_GENERIC_INST_FUNCTION;
    inst.flags = XG_GENERIC_INST_CONCRETE_TYPES | XG_GENERIC_INST_SPECIALIZED_BODY;
    ASSERT_NOT_NULL(xg_global_evidence_add_generic_inst(ev, &inst));
}

static void add_sample_semantic_summary(XgGlobalEvidence *ev) {
    XgDeclSummary decl = {0};
    XgClassSummary cls = {0};
    XgClassFieldSummary class_field = {0};
    XgMethodSummary method = {0};
    XgInterfaceImplSummary impl = {0};
    XgInterfaceExtendsSummary edge = {0};
    XgInterfaceMethodSummary iface_method = {0};
    XgDeriveSummary derive = {0};
    XgDerivedFieldSummary field = {0};
    XgDerivedMethodSummary derived_method = {0};

    add_sample_module_summary(ev);

    decl.decl_id = 10;
    decl.module_id = 7;
    decl.kind = XG_DECL_CLASS;
    decl.flags = XG_DECL_FINAL | XG_DECL_DERIVE;
    decl.name_id = xg_name_id("Box");
    decl.type_key = 900;
    decl.signature_key = 901;
    decl.source_span_id = 8;
    decl.derive_flags = XG_DERIVE_OPT_IN;
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(ev, &decl));

    cls.class_id = 20;
    cls.module_id = 7;
    cls.decl_id = decl.decl_id;
    cls.name_id = decl.name_id;
    cls.flags = XG_CLASS_EXPLICIT_FINAL | XG_CLASS_INFERRED_FINAL;
    cls.field_start = 1;
    cls.field_count = 1;
    cls.method_start = 30;
    cls.method_count = 1;
    cls.interface_start = 40;
    cls.interface_count = 1;
    cls.generic_origin_class_id = 19;
    cls.generic_origin_name_id = xg_name_id("Box");
    cls.generic_type_key = 900;
    cls.generic_type_arg_key_start = 910;
    cls.generic_type_arg_count = 1;
    cls.decl_kind = XG_DECL_CLASS;
    ASSERT_NOT_NULL(xg_global_evidence_add_class(ev, &cls));

    class_field.field_id = 1;
    class_field.module_id = 7;
    class_field.source_node_id = 10020;
    class_field.owner_class_id = cls.class_id;
    class_field.name_id = xg_name_id("item");
    class_field.type_key = 940;
    class_field.target_name_id = cls.name_id;
    class_field.target_class_id = cls.class_id;
    class_field.decl_ordinal = 0;
    class_field.instance_slot = 0;
    class_field.flags = XG_CLASS_FIELD_OWNED_REF;
    class_field.semantic_kind = XG_CLASS_FIELD_TYPE_CLASS;
    ASSERT_NOT_NULL(xg_global_evidence_add_class_field(ev, &class_field));

    method.method_id = 30;
    method.owner_class_id = cls.class_id;
    method.source_node_id = 10030;
    method.name_id = xg_name_id("value");
    method.signature_key = 920;
    method.root_method_id = method.method_id;
    method.flags = XG_METHOD_DIRECT_ONLY;
    ASSERT_NOT_NULL(xg_global_evidence_add_method(ev, &method));

    impl.implementor_class_id = cls.class_id;
    impl.interface_id = 41;
    impl.name_id = xg_name_id("Readable");
    impl.type_key = 930;
    impl.source_span_id = 9;
    impl.flags = 0x4;
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_impl(ev, &impl));

    edge.child_interface_id = 42;
    edge.parent_interface_id = 41;
    edge.name_id = xg_name_id("Parent");
    edge.type_key = 931;
    edge.source_span_id = 10;
    edge.flags = 0x5;
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_extends(ev, &edge));

    iface_method.interface_method_id = 50;
    iface_method.owner_interface_id = 41;
    iface_method.name_id = method.name_id;
    iface_method.signature_key = method.signature_key;
    iface_method.ordinal = 0;
    iface_method.source_span_id = 11;
    iface_method.flags = 0x6;
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_method(ev, &iface_method));

    derive.derive_id = 60;
    derive.module_id = 7;
    derive.owner_decl_id = decl.decl_id;
    derive.source_span_id = 12;
    derive.type_key = decl.type_key;
    derive.derive_kind = XG_DERIVE_HASH;
    derive.field_start = 70;
    derive.field_count = 1;
    derive.method_start = 80;
    derive.method_count = 1;
    derive.flags = XG_DERIVE_OPT_IN | XG_DERIVE_REACHABLE;
    derive.derive_hash = UINT64_C(0x8888);
    ASSERT_NOT_NULL(xg_global_evidence_add_derive(ev, &derive));

    field.field_id = 70;
    field.derive_id = derive.derive_id;
    field.field_ordinal = 0;
    field.name_id = class_field.name_id;
    field.type_key = class_field.type_key;
    field.source_field_id = class_field.field_id;
    field.flags = XG_DERIVED_FIELD_PUBLIC;
    ASSERT_NOT_NULL(xg_global_evidence_add_derived_field(ev, &field));

    derived_method.method_id = 80;
    derived_method.derive_id = derive.derive_id;
    derived_method.method_kind = XG_DERIVED_METHOD_HASH;
    derived_method.generated_body_func_id = 81;
    derived_method.signature_key = 950;
    derived_method.flags = XG_DERIVED_METHOD_NO_ALLOC | XG_DERIVED_METHOD_PURE;
    ASSERT_NOT_NULL(xg_global_evidence_add_derived_method(ev, &derived_method));
}

static void add_sample_global_extra_summary(XgGlobalEvidence *ev) {
    XgGenericBodyUseSummary body_use = {0};
    XgGenericStorageSummary storage = {0};
    XgGenericCodeSizeSummary code_size = {0};
    XgSequenceAccessSummary seq = {0};
    XgCapacityOpSummary cap = {0};
    XgBulkOpSummary bulk = {0};
    XgEncodingOpSummary enc = {0};
    XgJsonShapeSummary json_shape = {0};
    XgJsonFieldSummary json_field = {0};
    XgJsonAccessSummary json_access = {0};
    XgJsonCodecSummary json_codec = {0};
    XgRecordShapeSummary record_shape = {0};
    XgRecordFieldSummary record_field = {0};
    XgRecordAccessSummary record_access = {0};
    XgRecordMergeSummary record_merge = {0};
    XgOptionsBagSummary options = {0};
    XgMapShapeSummary map_shape = {0};
    XgMapEntrySummary map_entry = {0};
    XgKeyAccessSummary key_access = {0};
    XgHashEqSummary hash_eq = {0};

    body_use.use_id = 91;
    body_use.generic_inst_id = 3;
    body_use.module_id = 7;
    body_use.owner_func_id = 11;
    body_use.origin_body_func_id = 11;
    body_use.specialized_body_func_id = 31;
    body_use.root_callsite_id = 1;
    body_use.type_key = 400;
    body_use.type_arg_key_start = 401;
    body_use.type_arg_count = 1;
    body_use.estimated_body_size = 12;
    body_use.flags = XG_GENERIC_BODY_EXPLICIT_ROOT;
    body_use.body_use_hash = UINT64_C(0xabcd);
    ASSERT_NOT_NULL(xg_global_evidence_add_generic_body_use(ev, &body_use));

    storage.storage_id = 92;
    storage.generic_inst_id = 3;
    storage.module_id = 7;
    storage.storage_kind = XG_GENERIC_STORAGE_ARRAY;
    storage.origin_type_key = 400;
    storage.specialized_type_key = 401;
    storage.elem_type_key = 402;
    storage.container_plan_id = 55;
    storage.flags = XG_GENERIC_STORAGE_TYPED_INLINE | XG_GENERIC_STORAGE_POD;
    storage.storage_hash = UINT64_C(0xbcde);
    ASSERT_NOT_NULL(xg_global_evidence_add_generic_storage(ev, &storage));

    code_size.code_size_id = 93;
    code_size.generic_inst_id = 3;
    code_size.module_id = 7;
    code_size.body_use_id = 91;
    code_size.origin_body_size_estimate = 20;
    code_size.specialized_body_size_estimate = 12;
    code_size.instantiation_count = 1;
    code_size.threshold = 64;
    code_size.flags = XG_GENERIC_CODESIZE_ALLOW_CLONE;
    ASSERT_NOT_NULL(xg_global_evidence_add_generic_code_size(ev, &code_size));

    seq.access_id = 94;
    seq.owner_func_id = 11;
    seq.source_span_id = 21;
    seq.body_ordinal = 3;
    seq.sequence_kind = XG_SEQ_ARRAY;
    seq.access_kind = XG_SEQ_ACCESS_INDEX_GET;
    seq.receiver_type_key = 500;
    seq.elem_type_key = 501;
    seq.index_expr_id = 502;
    seq.length_expr_id = 503;
    seq.flags = XG_SEQ_ACCESS_CONST_INDEX;
    ASSERT_NOT_NULL(xg_global_evidence_add_sequence_access(ev, &seq));

    cap.op_id = 95;
    cap.owner_func_id = 11;
    cap.source_span_id = 22;
    cap.body_ordinal = 4;
    cap.sequence_kind = XG_SEQ_STRING_BUILDER;
    cap.op_kind = XG_CAPACITY_PUSH;
    cap.receiver_type_key = 510;
    cap.elem_type_key = 511;
    cap.count_expr_id = 512;
    cap.loop_id = 513;
    cap.flags = XG_CAPACITY_MAY_GROW;
    ASSERT_NOT_NULL(xg_global_evidence_add_capacity_op(ev, &cap));

    bulk.op_id = 96;
    bulk.owner_func_id = 11;
    bulk.source_span_id = 23;
    bulk.body_ordinal = 5;
    bulk.op_kind = XG_BULK_COPY;
    bulk.elem_type_key = 520;
    bulk.src_type_key = 521;
    bulk.dst_type_key = 522;
    bulk.length_expr_id = 523;
    bulk.flags = XG_BULK_POD;
    ASSERT_NOT_NULL(xg_global_evidence_add_bulk_op(ev, &bulk));

    enc.op_id = 97;
    enc.owner_func_id = 11;
    enc.source_span_id = 24;
    enc.body_ordinal = 6;
    enc.op_kind = XG_ENCODING_STRING_TO_BYTES;
    enc.input_type_key = 530;
    enc.output_type_key = 531;
    enc.flags = XG_ENCODING_KNOWN_UTF8;
    ASSERT_NOT_NULL(xg_global_evidence_add_encoding_op(ev, &enc));

    json_shape.json_shape_id = 101;
    json_shape.module_id = 7;
    json_shape.owner_func_id = 11;
    json_shape.source_span_id = 25;
    json_shape.type_key = 600;
    json_shape.field_name_start = 610;
    json_shape.field_count = 1;
    json_shape.shape_kind = XG_JSON_SHAPE_SHAPED;
    json_shape.flags = XG_JSON_SHAPE_STATIC_KEYS;
    json_shape.shape_hash = UINT64_C(0xcafe);
    ASSERT_NOT_NULL(xg_global_evidence_add_json_shape(ev, &json_shape));

    json_field.field_id = 102;
    json_field.shape_id = 101;
    json_field.field_ordinal = 0;
    json_field.name_id = xg_name_id("name");
    json_field.type_key = 601;
    json_field.flags = XG_JSON_FIELD_STATIC_KEY;
    ASSERT_NOT_NULL(xg_global_evidence_add_json_field(ev, &json_field));

    json_access.json_access_id = 103;
    json_access.module_id = 7;
    json_access.owner_func_id = 11;
    json_access.receiver_shape_id = 101;
    json_access.source_span_id = 26;
    json_access.key_name_id = json_field.name_id;
    json_access.result_type_key = 601;
    json_access.field_ordinal = 0;
    json_access.access_kind = XG_JSON_ACCESS_FIELD_GET;
    json_access.flags = XG_JSON_ACCESS_STATIC_KEY | XG_JSON_ACCESS_RECEIVER_SHAPE_PROVEN;
    ASSERT_NOT_NULL(xg_global_evidence_add_json_access(ev, &json_access));

    json_codec.codec_id = 104;
    json_codec.module_id = 7;
    json_codec.owner_func_id = 11;
    json_codec.source_span_id = 27;
    json_codec.codec_kind = XG_JSON_CODEC_DECODE;
    json_codec.input_type_key = 600;
    json_codec.target_type_key = 601;
    json_codec.input_shape_id = 101;
    json_codec.output_shape_id = 101;
    json_codec.field_count = 1;
    json_codec.flags = XG_JSON_CODEC_HAS_INPUT_SHAPE | XG_JSON_CODEC_HAS_TARGET_TYPE;
    ASSERT_NOT_NULL(xg_global_evidence_add_json_codec(ev, &json_codec));

    record_shape.record_shape_id = 201;
    record_shape.module_id = 7;
    record_shape.owner_func_id = 11;
    record_shape.source_span_id = 28;
    record_shape.type_key = 700;
    record_shape.field_name_start = 710;
    record_shape.field_count = 1;
    record_shape.shape_kind = XG_RECORD_SHAPE_LITERAL;
    record_shape.flags = XG_RECORD_SHAPE_STATIC_KEYS;
    record_shape.shape_hash = UINT64_C(0xd00d);
    ASSERT_NOT_NULL(xg_global_evidence_add_record_shape(ev, &record_shape));

    record_field.field_id = 202;
    record_field.shape_id = 201;
    record_field.field_ordinal = 0;
    record_field.name_id = xg_name_id("value");
    record_field.type_key = 701;
    record_field.default_value_id = 702;
    record_field.flags = XG_RECORD_FIELD_REQUIRED;
    ASSERT_NOT_NULL(xg_global_evidence_add_record_field(ev, &record_field));

    record_access.record_access_id = 203;
    record_access.module_id = 7;
    record_access.owner_func_id = 11;
    record_access.receiver_shape_id = 201;
    record_access.source_span_id = 29;
    record_access.field_name_id = record_field.name_id;
    record_access.result_type_key = 701;
    record_access.field_ordinal = 0;
    record_access.access_kind = XG_RECORD_ACCESS_FIELD_GET;
    record_access.flags = XG_RECORD_ACCESS_STATIC_FIELD | XG_RECORD_ACCESS_RECEIVER_SHAPE_PROVEN;
    ASSERT_NOT_NULL(xg_global_evidence_add_record_access(ev, &record_access));

    record_merge.merge_id = 204;
    record_merge.module_id = 7;
    record_merge.owner_func_id = 11;
    record_merge.source_span_id = 30;
    record_merge.base_shape_id = 201;
    record_merge.patch_shape_id = 201;
    record_merge.result_shape_id = 201;
    record_merge.base_field_count = 1;
    record_merge.patch_field_count = 1;
    record_merge.result_field_count = 1;
    record_merge.overwrite_count = 1;
    record_merge.copy_table_id = 800;
    record_merge.flags = XG_RECORD_MERGE_BASE_SHAPE_PROVEN | XG_RECORD_MERGE_RESULT_SHAPE_PROVEN;
    record_merge.merge_hash = UINT64_C(0xd0d0);
    ASSERT_NOT_NULL(xg_global_evidence_add_record_merge(ev, &record_merge));

    options.options_id = 205;
    options.module_id = 7;
    options.owner_func_id = 11;
    options.callsite_id = 1;
    options.param_shape_id = 201;
    options.supplied_shape_id = 201;
    options.source_span_id = 31;
    options.supplied_field_mask_id = 810;
    options.default_field_mask_id = 811;
    options.required_field_mask_id = 812;
    options.supplied_count = 1;
    options.default_count = 0;
    options.required_count = 1;
    options.action = XG_OPTIONS_DEFAULT_ELIDED;
    options.flags = XG_OPTIONS_ALL_SUPPLIED | XG_OPTIONS_CALLSITE_PROVEN;
    ASSERT_NOT_NULL(xg_global_evidence_add_options_bag(ev, &options));

    map_shape.shape_id = 301;
    map_shape.module_id = 7;
    map_shape.owner_func_id = 11;
    map_shape.source_span_id = 32;
    map_shape.container_kind = XG_MAP_CONTAINER_MAP;
    map_shape.source = XG_MAP_SHAPE_SRC_LITERAL;
    map_shape.key_type_key = 900;
    map_shape.value_type_key = 901;
    map_shape.entry_start = 302;
    map_shape.entry_count = 1;
    map_shape.literal_count = 1;
    map_shape.flags = XG_MAP_SHAPE_LITERAL | XG_MAP_SHAPE_SMALL;
    map_shape.shape_hash = UINT64_C(0xe00e);
    ASSERT_NOT_NULL(xg_global_evidence_add_map_shape(ev, &map_shape));

    map_entry.entry_id = 302;
    map_entry.shape_id = 301;
    map_entry.entry_ordinal = 0;
    map_entry.key_const_id = 910;
    map_entry.value_const_id = 911;
    map_entry.key_i64 = -7;
    map_entry.prehash = UINT64_C(0xee00);
    map_entry.flags = XG_MAP_ENTRY_CONST_KEY | XG_MAP_ENTRY_INT_KEY;
    ASSERT_NOT_NULL(xg_global_evidence_add_map_entry(ev, &map_entry));

    key_access.access_id = 303;
    key_access.owner_func_id = 11;
    key_access.source_span_id = 33;
    key_access.body_ordinal = 7;
    key_access.container_kind = XG_MAP_CONTAINER_MAP;
    key_access.op = XG_KEY_ACCESS_INDEX_GET;
    key_access.receiver_shape_id = 301;
    key_access.receiver_type_key = 920;
    key_access.key_type_key = 900;
    key_access.value_type_key = 901;
    key_access.key_const_id = 910;
    key_access.key_prehash = UINT64_C(0xee00);
    key_access.flags = XG_KEY_ACCESS_CONST_KEY;
    ASSERT_NOT_NULL(xg_global_evidence_add_key_access(ev, &key_access));

    hash_eq.hash_eq_id = 304;
    hash_eq.type_key = 900;
    hash_eq.kind = XG_HASH_EQ_BUILTIN;
    hash_eq.eq_derive_id = 60;
    hash_eq.hash_derive_id = 60;
    hash_eq.eq_func_id = 11;
    hash_eq.hash_func_id = 31;
    hash_eq.flags = XG_HASH_EQ_NO_ALLOC | XG_HASH_EQ_PURE;
    ASSERT_NOT_NULL(xg_global_evidence_add_hash_eq(ev, &hash_eq));
}

TEST(cache_payload_parse_exposes_validated_body) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence materialized = {0};
    XgEvidenceCachePayloadInfo info;
    XgEvidenceCacheRequestKey expected_request;
    XgEvidenceCacheRequestKey expected_request_from_key;
    XgEvidenceCacheKey expected;
    XgEvidenceCacheKey materialized_key;
    char *payload;

    ev.key.module_id = 7;
    ev.key.source_hash = UINT64_C(0x10101010);
    ev.key.profile = XG_BUILD_NATIVE_RELEASE;
    ev.key.compiler_semver_hash = UINT64_C(0x1111);
    ev.key.profile_hash = UINT64_C(0x2222);
    ev.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_body_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    ASSERT(xg_evidence_cache_payload_parse(payload, &info));
    expected_request = xg_global_evidence_cache_request_key(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    expected_request_from_key =
        xg_evidence_cache_request_key_from_build_key(&ev.key, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT(xg_evidence_cache_request_key_matches(&info.request_key, &expected_request));
    ASSERT(xg_evidence_cache_request_key_matches(&expected_request_from_key, &expected_request));
    ASSERT(xg_evidence_cache_key_matches(&info.key, &expected));
    ASSERT_EQ_UINT(info.phase, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_EQ_UINT(info.request_hash, xg_evidence_cache_request_key_hash(&expected_request));
    ASSERT_EQ_UINT(info.key_hash, xg_evidence_cache_key_hash(&expected));
    ASSERT_EQ_UINT(info.body_len, info.payload_bytes);
    ASSERT_NOT_NULL(strstr(payload, "xg-cache-payload v2"));
    ASSERT_NOT_NULL(strstr(payload, "xg-cache-request v1"));
    ASSERT_NOT_NULL(strstr(info.body, "payload-count bodies=1 callsites=1"));
    ASSERT_NOT_NULL(strstr(info.body, "interface_object_uses=1"));
    ASSERT_NOT_NULL(strstr(info.body, "body id=11"));
    ASSERT_NOT_NULL(strstr(info.body, "interface-object-use id=4 interface=41"));
    ASSERT(xg_evidence_cache_payload_request_matches(payload, &expected_request));
    ASSERT(xg_evidence_cache_payload_matches(payload, &expected));
    ASSERT(xg_evidence_cache_payload_materialize(payload, &materialized));
    ASSERT_EQ_UINT(materialized.key.source_hash, ev.key.source_hash);
    materialized_key = xg_global_evidence_cache_key(&materialized, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT(xg_evidence_cache_key_matches(&materialized_key, &expected));
    ASSERT_EQ_UINT(materialized.nbodies, 1);
    ASSERT_EQ_UINT(materialized.ncallsites, 1);
    ASSERT_EQ_UINT(materialized.ninterface_object_uses, 1);
    ASSERT_EQ_UINT(materialized.nlink_deps, 1);
    ASSERT_EQ_UINT(materialized.ngeneric_insts, 1);
    ASSERT_EQ_UINT(materialized.bodies[0].func_id, 11);
    ASSERT_EQ_UINT(materialized.callsites[0].callsite_id, 1);
    ASSERT_EQ_UINT(materialized.interface_object_uses[0].interface_id, 41);
    ASSERT_EQ_UINT(materialized.interface_object_uses[0].reason,
                   XG_INTERFACE_OBJECT_USE_VALUE | XG_INTERFACE_OBJECT_USE_PARAM);
    ASSERT_STR_EQ(materialized.link_deps[0].name, "mem.copy");
    ASSERT_EQ_UINT(materialized.generic_insts[0].specialized_func_id, 31);

    xg_global_evidence_free(&materialized);
    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_materializes_declaration_summary) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence materialized = {0};
    XgEvidenceCacheKey expected;
    XgEvidenceCacheKey materialized_key;
    char *payload;

    ev.key.module_id = 7;
    ev.key.profile = XG_BUILD_NATIVE_RELEASE;
    ev.key.compiler_semver_hash = UINT64_C(0x1111);
    ev.key.profile_hash = UINT64_C(0x2222);
    ev.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_module_summary(&ev);
    add_sample_body_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT_NOT_NULL(payload);
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(xg_evidence_cache_payload_materialize(payload, &materialized));
    materialized_key = xg_global_evidence_cache_key(&materialized, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(xg_evidence_cache_key_matches(&materialized_key, &expected));
    ASSERT_EQ_UINT(materialized.nmodules, 1);
    ASSERT_EQ_UINT(materialized.ndecls, 1);
    ASSERT_EQ_UINT(materialized.modules[0].module_id, 7);
    ASSERT_EQ_UINT(materialized.modules[0].source_hash, UINT64_C(0xdef7));
    ASSERT_EQ_UINT(materialized.decls[0].decl_id, 1);
    ASSERT_EQ_UINT(materialized.decls[0].signature_key, 101);

    xg_global_evidence_free(&materialized);
    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_materializes_semantic_graph_summary) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence materialized = {0};
    XgEvidenceCacheKey expected;
    XgEvidenceCacheKey materialized_key;
    char *payload;

    ev.key.module_id = 7;
    ev.key.profile = XG_BUILD_NATIVE_RELEASE;
    ev.key.compiler_semver_hash = UINT64_C(0x1111);
    ev.key.profile_hash = UINT64_C(0x2222);
    ev.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_semantic_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_SEMANTIC_GRAPH);
    ASSERT_NOT_NULL(payload);
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_SEMANTIC_GRAPH);
    ASSERT(xg_evidence_cache_payload_materialize(payload, &materialized));
    materialized_key =
        xg_global_evidence_cache_key(&materialized, XG_EVIDENCE_CACHE_SEMANTIC_GRAPH);
    ASSERT(xg_evidence_cache_key_matches(&materialized_key, &expected));
    ASSERT_EQ_UINT(materialized.nmodules, 1);
    ASSERT_EQ_UINT(materialized.ndecls, 1);
    ASSERT_EQ_UINT(materialized.nclasses, 1);
    ASSERT_EQ_UINT(materialized.nclass_fields, 1);
    ASSERT_EQ_UINT(materialized.nmethods, 1);
    ASSERT_EQ_UINT(materialized.ninterface_impls, 1);
    ASSERT_EQ_UINT(materialized.ninterface_extends, 1);
    ASSERT_EQ_UINT(materialized.ninterface_methods, 1);
    ASSERT_EQ_UINT(materialized.nderives, 1);
    ASSERT_EQ_UINT(materialized.nderived_fields, 1);
    ASSERT_EQ_UINT(materialized.nderived_methods, 1);
    ASSERT_EQ_UINT(materialized.modules[0].module_id, 7);
    ASSERT_EQ_UINT(materialized.modules[0].canonical_hash, UINT64_C(0xabc7));
    ASSERT_EQ_UINT(materialized.classes[0].generic_type_arg_count, 1);
    ASSERT_EQ_UINT(materialized.classes[0].field_start, 1);
    ASSERT_EQ_UINT(materialized.classes[0].field_count, 1);
    ASSERT_EQ_UINT(materialized.class_fields[0].field_id, 1);
    ASSERT_EQ_UINT(materialized.class_fields[0].source_node_id, 10020);
    ASSERT_EQ_UINT(materialized.class_fields[0].owner_class_id, 20);
    ASSERT_EQ_UINT(materialized.class_fields[0].target_class_id, 20);
    ASSERT_EQ_UINT(materialized.methods[0].root_method_id, 30);
    ASSERT_EQ_UINT(materialized.methods[0].source_node_id, 10030);
    ASSERT_EQ_UINT(materialized.derives[0].derive_hash, UINT64_C(0x8888));
    ASSERT_EQ_UINT(materialized.derived_fields[0].source_field_id, 1);

    xg_global_evidence_free(&materialized);
    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_parse_rejects_body_drift) {
    XgGlobalEvidence ev = {0};
    XgEvidenceCachePayloadInfo info;
    char *payload;
    char *needle;

    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);

    needle = strstr(payload, "body id=11");
    ASSERT_NOT_NULL(needle);
    needle[8] = '2';
    ASSERT(!xg_evidence_cache_payload_parse(payload, &info));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_matches_rejects_wrong_phase_key) {
    XgGlobalEvidence ev = {0};
    XgEvidenceCacheKey wrong_phase;
    XgEvidenceCacheRequestKey wrong_request;
    char *payload;

    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    wrong_phase = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    wrong_request = xg_global_evidence_cache_request_key(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(!xg_evidence_cache_payload_request_matches(payload, &wrong_request));
    ASSERT(!xg_evidence_cache_payload_matches(payload, &wrong_phase));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_parse_rejects_request_drift) {
    XgGlobalEvidence ev = {0};
    XgEvidenceCachePayloadInfo info;
    char *payload;
    char *needle;

    ev.key.source_hash = UINT64_C(0x7777);
    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);

    needle = strstr(payload, " source=0000000000007777");
    ASSERT_NOT_NULL(needle);
    needle[23] = '8';
    ASSERT(!xg_evidence_cache_payload_parse(payload, &info));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_materializes_global_evidence) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence materialized = {0};
    XgEvidenceCacheKey expected;
    XgEvidenceCacheKey materialized_key;
    char *payload;

    ev.key.module_id = 7;
    ev.key.source_hash = UINT64_C(0x10101010);
    ev.key.profile = XG_BUILD_NATIVE_RELEASE;
    ev.key.compiler_semver_hash = UINT64_C(0x1111);
    ev.key.profile_hash = UINT64_C(0x2222);
    ev.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_semantic_summary(&ev);
    add_sample_body_summary(&ev);
    add_sample_global_extra_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT_NOT_NULL(payload);
    ASSERT_NOT_NULL(strstr(payload, "payload-global v1"));
    ASSERT_NOT_NULL(strstr(payload, "payload-extra v1 generic_body_uses=1"));
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT(xg_evidence_cache_payload_materialize(payload, &materialized));
    materialized_key =
        xg_global_evidence_cache_key(&materialized, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT(xg_evidence_cache_key_matches(&materialized_key, &expected));
    ASSERT_EQ_UINT(materialized.nmodules, 1);
    ASSERT_EQ_UINT(materialized.ndecls, 2);
    ASSERT_EQ_UINT(materialized.nclasses, 1);
    ASSERT_EQ_UINT(materialized.nbodies, 1);
    ASSERT_EQ_UINT(materialized.ngeneric_body_uses, 1);
    ASSERT_EQ_UINT(materialized.ngeneric_storages, 1);
    ASSERT_EQ_UINT(materialized.ngeneric_code_sizes, 1);
    ASSERT_EQ_UINT(materialized.nsequence_accesses, 1);
    ASSERT_EQ_UINT(materialized.ncapacity_ops, 1);
    ASSERT_EQ_UINT(materialized.nbulk_ops, 1);
    ASSERT_EQ_UINT(materialized.nencoding_ops, 1);
    ASSERT_EQ_UINT(materialized.njson_shapes, 1);
    ASSERT_EQ_UINT(materialized.njson_fields, 1);
    ASSERT_EQ_UINT(materialized.njson_accesses, 1);
    ASSERT_EQ_UINT(materialized.njson_codecs, 1);
    ASSERT_EQ_UINT(materialized.nrecord_shapes, 1);
    ASSERT_EQ_UINT(materialized.nrecord_fields, 1);
    ASSERT_EQ_UINT(materialized.nrecord_accesses, 1);
    ASSERT_EQ_UINT(materialized.nrecord_merges, 1);
    ASSERT_EQ_UINT(materialized.noptions_bags, 1);
    ASSERT_EQ_UINT(materialized.nmap_shapes, 1);
    ASSERT_EQ_UINT(materialized.nmap_entries, 1);
    ASSERT_EQ_UINT(materialized.nkey_accesses, 1);
    ASSERT_EQ_UINT(materialized.nhash_eqs, 1);
    ASSERT_EQ_UINT(materialized.modules[0].flags, XG_MODULE_EMBEDDED_SOURCE);
    ASSERT_EQ_UINT(materialized.generic_body_uses[0].use_id, 91);
    ASSERT_EQ_UINT(materialized.json_shapes[0].json_shape_id, 101);
    ASSERT_EQ_UINT(materialized.record_merges[0].merge_id, 204);
    ASSERT_EQ_UINT(materialized.key_accesses[0].access_id, 303);

    xg_global_evidence_free(&materialized);
    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_imports_package_with_id_remap) {
    XgGlobalEvidence package = {0};
    XgGlobalEvidence target = {0};
    XgEvidencePackageImportReport report;
    XgModuleSummary consumer_module = {0};
    XgDeclSummary consumer_decl = {0};
    XgClassSummary consumer_class = {0};
    XgClassFieldSummary consumer_field = {0};
    XgMethodSummary consumer_method = {0};
    XgInterfaceObjectUseSummary consumer_iface_use = {0};
    XgBodySummary consumer_body = {0};
    XgCallsiteSummary consumer_call = {0};
    XgJsonShapeSummary consumer_json = {0};
    XgMapShapeSummary consumer_map = {0};
    XgMapEntrySummary consumer_entry = {0};
    uint64_t package_hash;
    char *payload;

    package.key.module_id = 7;
    package.key.source_hash = UINT64_C(0x10101010);
    package.key.profile = XG_BUILD_NATIVE_RELEASE;
    package.key.compiler_semver_hash = UINT64_C(0x1111);
    package.key.profile_hash = UINT64_C(0x2222);
    package.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_semantic_summary(&package);
    add_sample_body_summary(&package);
    add_sample_global_extra_summary(&package);
    package.classes[0].method_start = 1;
    package.classes[0].interface_start = 1;
    package.derives[0].field_start = 1;
    package.derives[0].method_start = 1;
    package.map_shapes[0].entry_start = 1;
    package_hash = xg_global_evidence_hash(&package);
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT_NOT_NULL(payload);

    consumer_module.module_id = 100;
    consumer_module.name_id = xg_name_id("consumer");
    consumer_module.canonical_hash = UINT64_C(0xc001);
    consumer_module.source_hash = UINT64_C(0xc002);
    consumer_module.kind = 1;
    ASSERT_NOT_NULL(xg_global_evidence_add_module(&target, &consumer_module));

    consumer_decl.decl_id = 100;
    consumer_decl.module_id = consumer_module.module_id;
    consumer_decl.kind = XG_DECL_FUNC;
    consumer_decl.name_id = xg_name_id("main");
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&target, &consumer_decl));

    consumer_class.class_id = 200;
    consumer_class.module_id = consumer_module.module_id;
    consumer_class.decl_id = consumer_decl.decl_id;
    consumer_class.name_id = xg_name_id("Consumer");
    consumer_class.field_start = 1;
    consumer_class.field_count = 1;
    consumer_class.method_start = 1;
    consumer_class.method_count = 1;
    ASSERT_NOT_NULL(xg_global_evidence_add_class(&target, &consumer_class));

    consumer_field.field_id = 300;
    consumer_field.module_id = consumer_module.module_id;
    consumer_field.owner_class_id = consumer_class.class_id;
    consumer_field.name_id = xg_name_id("value");
    consumer_field.semantic_kind = XG_CLASS_FIELD_TYPE_I64;
    ASSERT_NOT_NULL(xg_global_evidence_add_class_field(&target, &consumer_field));

    consumer_method.method_id = 400;
    consumer_method.owner_class_id = consumer_class.class_id;
    consumer_method.name_id = xg_name_id("value");
    consumer_method.root_method_id = consumer_method.method_id;
    ASSERT_NOT_NULL(xg_global_evidence_add_method(&target, &consumer_method));

    consumer_body.func_id = 500;
    consumer_body.module_id = consumer_module.module_id;
    consumer_body.owner_decl_id = consumer_decl.decl_id;
    consumer_body.name_id = consumer_decl.name_id;
    consumer_body.kind = XG_BODY_FUNCTION;
    consumer_body.callsite_start = 401;
    consumer_body.callsite_count = 1;
    ASSERT_NOT_NULL(xg_global_evidence_add_body(&target, &consumer_body));

    consumer_call.callsite_id = 401;
    consumer_call.owner_func_id = consumer_body.func_id;
    consumer_call.kind = XG_CALL_CLOSURE;
    ASSERT_NOT_NULL(xg_global_evidence_add_callsite(&target, &consumer_call));

    consumer_iface_use.use_id = 650;
    consumer_iface_use.interface_id = 700;
    consumer_iface_use.owner_func_id = consumer_body.func_id;
    consumer_iface_use.reason = XG_INTERFACE_OBJECT_USE_VALUE;
    ASSERT_NOT_NULL(xg_global_evidence_add_interface_object_use(&target, &consumer_iface_use));

    consumer_json.json_shape_id = 800;
    consumer_json.module_id = consumer_module.module_id;
    consumer_json.owner_func_id = consumer_body.func_id;
    consumer_json.shape_kind = XG_JSON_SHAPE_SHAPED;
    ASSERT_NOT_NULL(xg_global_evidence_add_json_shape(&target, &consumer_json));

    consumer_map.shape_id = 900;
    consumer_map.module_id = consumer_module.module_id;
    consumer_map.owner_func_id = consumer_body.func_id;
    consumer_map.container_kind = XG_MAP_CONTAINER_MAP;
    consumer_map.source = XG_MAP_SHAPE_SRC_LITERAL;
    consumer_map.entry_start = 1;
    consumer_map.entry_count = 1;
    ASSERT_NOT_NULL(xg_global_evidence_add_map_shape(&target, &consumer_map));

    consumer_entry.entry_id = 950;
    consumer_entry.shape_id = consumer_map.shape_id;
    ASSERT_NOT_NULL(xg_global_evidence_add_map_entry(&target, &consumer_entry));

    ASSERT(xg_global_evidence_import_package_payload(&target, payload, &report));
    ASSERT_EQ_UINT(report.package_hash, package_hash);
    ASSERT_EQ_UINT(report.modules_remapped, 1);
    ASSERT_EQ_UINT(report.modules_added, 1);
    ASSERT_EQ_UINT(report.rows_imported, 36);

    ASSERT_EQ_UINT(target.nmodules, 2);
    ASSERT_EQ_UINT(target.modules[1].module_id, 101);
    ASSERT_EQ_UINT(target.modules[1].canonical_hash, UINT64_C(0xabc7));
    ASSERT_EQ_UINT(target.ndecls, 3);
    ASSERT_EQ_UINT(target.decls[1].decl_id, 110);
    ASSERT_EQ_UINT(target.decls[1].module_id, 101);
    ASSERT_EQ_UINT(target.decls[2].decl_id, 101);
    ASSERT_EQ_UINT(target.decls[2].module_id, 101);
    ASSERT_EQ_UINT(target.nclasses, 2);
    ASSERT_EQ_UINT(target.classes[1].class_id, 220);
    ASSERT_EQ_UINT(target.classes[1].decl_id, 110);
    ASSERT_EQ_UINT(target.classes[1].field_start, 2);
    ASSERT_EQ_UINT(target.classes[1].method_start, 2);
    ASSERT_EQ_UINT(target.classes[1].interface_start, 1);
    ASSERT_EQ_UINT(target.class_fields[1].field_id, 301);
    ASSERT_EQ_UINT(target.class_fields[1].module_id, 101);
    ASSERT_EQ_UINT(target.class_fields[1].owner_class_id, 220);
    ASSERT_EQ_UINT(target.class_fields[1].target_class_id, 220);
    ASSERT_EQ_UINT(target.methods[1].method_id, 430);
    ASSERT_EQ_UINT(target.methods[1].root_method_id, 430);
    ASSERT_EQ_UINT(target.interface_impls[0].implementor_class_id, 220);
    ASSERT_EQ_UINT(target.interface_impls[0].interface_id, 741);
    ASSERT_EQ_UINT(target.interface_methods[0].interface_method_id, 50);
    ASSERT_EQ_UINT(target.interface_methods[0].owner_interface_id, 741);
    ASSERT_EQ_UINT(target.bodies[1].func_id, 511);
    ASSERT_EQ_UINT(target.bodies[1].module_id, 101);
    ASSERT_EQ_UINT(target.bodies[1].owner_decl_id, 101);
    ASSERT_EQ_UINT(target.bodies[1].callsite_start, 402);
    ASSERT_EQ_UINT(target.callsites[1].callsite_id, 402);
    ASSERT_EQ_UINT(target.callsites[1].owner_func_id, 511);
    ASSERT_EQ_UINT(target.callsites[1].static_target_func_id, 511);
    ASSERT_EQ_UINT(target.generic_insts[0].generic_inst_id, 3);
    ASSERT_EQ_UINT(target.generic_insts[0].module_id, 101);
    ASSERT_EQ_UINT(target.generic_insts[0].origin_decl_id, 101);
    ASSERT_EQ_UINT(target.generic_insts[0].origin_func_id, 511);
    ASSERT_EQ_UINT(target.generic_insts[0].specialized_func_id, 531);
    ASSERT_EQ_UINT(target.generic_insts[0].root_callsite_id, 402);
    ASSERT_EQ_UINT(target.generic_body_uses[0].generic_inst_id, 3);
    ASSERT_EQ_UINT(target.generic_body_uses[0].owner_func_id, 511);
    ASSERT_EQ_UINT(target.json_shapes[1].json_shape_id, 901);
    ASSERT_EQ_UINT(target.json_shapes[1].module_id, 101);
    ASSERT_EQ_UINT(target.json_accesses[0].receiver_shape_id, 901);
    ASSERT_EQ_UINT(target.map_shapes[1].shape_id, 1201);
    ASSERT_EQ_UINT(target.map_shapes[1].entry_start, 2);
    ASSERT_EQ_UINT(target.map_entries[1].entry_id, 1252);
    ASSERT_EQ_UINT(target.map_entries[1].shape_id, 1201);
    ASSERT_EQ_UINT(target.key_accesses[0].receiver_shape_id, 1201);
    ASSERT_EQ_UINT(target.hash_eqs[0].eq_derive_id, 60);
    ASSERT_EQ_UINT(target.hash_eqs[0].eq_func_id, 511);
    ASSERT_EQ_UINT(target.hash_eqs[0].hash_func_id, 531);

    xr_free(payload);
    xg_global_evidence_free(&target);
    xg_global_evidence_free(&package);
}

TEST(cache_payload_import_reuses_module_stub_without_duplicate_module) {
    XgGlobalEvidence package = {0};
    XgGlobalEvidence target = {0};
    XgEvidencePackageImportReport report;
    XgModuleSummary existing_module = {0};
    char *payload;

    package.key.module_id = 7;
    package.key.profile = XG_BUILD_NATIVE_RELEASE;
    add_sample_semantic_summary(&package);
    add_sample_body_summary(&package);
    add_sample_global_extra_summary(&package);
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT_NOT_NULL(payload);

    existing_module = package.modules[0];
    existing_module.module_id = 100;
    ASSERT_NOT_NULL(xg_global_evidence_add_module(&target, &existing_module));

    ASSERT(xg_global_evidence_import_package_payload(&target, payload, &report));
    ASSERT_EQ_UINT(report.modules_remapped, 1);
    ASSERT_EQ_UINT(report.modules_added, 0);
    ASSERT_EQ_UINT(report.rows_imported, 36);
    ASSERT_EQ_UINT(target.nmodules, 1);
    ASSERT_EQ_UINT(target.modules[0].module_id, 100);
    ASSERT_EQ_UINT(target.decls[0].module_id, 100);
    ASSERT_EQ_UINT(target.bodies[0].module_id, 100);
    ASSERT_EQ_UINT(target.json_shapes[0].module_id, 100);

    xr_free(payload);
    xg_global_evidence_free(&target);
    xg_global_evidence_free(&package);
}

TEST(cache_payload_import_rejects_duplicate_rows_for_existing_module) {
    XgGlobalEvidence package = {0};
    XgGlobalEvidence target = {0};
    XgModuleSummary existing_module = {0};
    XgDeclSummary existing_decl = {0};
    char *payload;

    package.key.module_id = 7;
    package.key.profile = XG_BUILD_NATIVE_RELEASE;
    add_sample_semantic_summary(&package);
    add_sample_body_summary(&package);
    add_sample_global_extra_summary(&package);
    payload = xg_global_evidence_cache_payload_dump(&package, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT_NOT_NULL(payload);

    existing_module = package.modules[0];
    existing_module.module_id = 100;
    ASSERT_NOT_NULL(xg_global_evidence_add_module(&target, &existing_module));
    existing_decl.decl_id = 1;
    existing_decl.module_id = existing_module.module_id;
    existing_decl.kind = XG_DECL_FUNC;
    existing_decl.name_id = xg_name_id("already_loaded");
    ASSERT_NOT_NULL(xg_global_evidence_add_decl(&target, &existing_decl));

    ASSERT(!xg_global_evidence_import_package_payload(&target, payload, NULL));
    ASSERT_EQ_UINT(target.nmodules, 1);
    ASSERT_EQ_UINT(target.ndecls, 1);

    xr_free(payload);
    xg_global_evidence_free(&target);
    xg_global_evidence_free(&package);
}

TEST(cache_payload_import_rejects_non_global_or_missing_module_identity) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence target = {0};
    char *payload;

    ev.key.module_id = 7;
    add_sample_semantic_summary(&ev);
    add_sample_body_summary(&ev);
    add_sample_global_extra_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    ASSERT(!xg_global_evidence_import_package_payload(&target, payload, NULL));
    xr_free(payload);
    xg_global_evidence_free(&ev);

    memset(&ev, 0, sizeof(ev));
    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT_NOT_NULL(payload);
    ASSERT(!xg_global_evidence_import_package_payload(&target, payload, NULL));
    xr_free(payload);
    xg_global_evidence_free(&ev);
    xg_global_evidence_free(&target);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Global Evidence Cache Payload");
RUN_TEST(cache_payload_parse_exposes_validated_body);
RUN_TEST(cache_payload_materializes_declaration_summary);
RUN_TEST(cache_payload_materializes_semantic_graph_summary);
RUN_TEST(cache_payload_parse_rejects_body_drift);
RUN_TEST(cache_payload_matches_rejects_wrong_phase_key);
RUN_TEST(cache_payload_parse_rejects_request_drift);
RUN_TEST(cache_payload_materializes_global_evidence);
RUN_TEST(cache_payload_imports_package_with_id_remap);
RUN_TEST(cache_payload_import_reuses_module_stub_without_duplicate_module);
RUN_TEST(cache_payload_import_rejects_duplicate_rows_for_existing_module);
RUN_TEST(cache_payload_import_rejects_non_global_or_missing_module_identity);

TEST_MAIN_END()
