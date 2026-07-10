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

TEST(cache_payload_parse_exposes_validated_body) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence materialized = {0};
    XgEvidenceCachePayloadInfo info;
    XgEvidenceCacheKey expected;
    XgEvidenceCacheKey materialized_key;
    char *payload;

    ev.key.module_id = 7;
    ev.key.profile = XG_BUILD_NATIVE_RELEASE;
    ev.key.compiler_semver_hash = UINT64_C(0x1111);
    ev.key.profile_hash = UINT64_C(0x2222);
    ev.key.imported_summary_hash = UINT64_C(0x3333);
    add_sample_body_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    ASSERT(xg_evidence_cache_payload_parse(payload, &info));
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT(xg_evidence_cache_key_matches(&info.key, &expected));
    ASSERT_EQ_UINT(info.phase, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_EQ_UINT(info.key_hash, xg_evidence_cache_key_hash(&expected));
    ASSERT_EQ_UINT(info.body_len, info.payload_bytes);
    ASSERT_NOT_NULL(strstr(info.body, "payload-count bodies=1 callsites=1"));
    ASSERT_NOT_NULL(strstr(info.body, "interface_object_uses=1"));
    ASSERT_NOT_NULL(strstr(info.body, "body id=11"));
    ASSERT_NOT_NULL(strstr(info.body, "interface-object-use id=4 interface=41"));
    ASSERT(xg_evidence_cache_payload_matches(payload, &expected));
    ASSERT(xg_evidence_cache_payload_materialize(payload, &materialized));
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
    add_sample_body_summary(&ev);

    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT_NOT_NULL(payload);
    expected = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(xg_evidence_cache_payload_materialize(payload, &materialized));
    materialized_key = xg_global_evidence_cache_key(&materialized, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(xg_evidence_cache_key_matches(&materialized_key, &expected));
    ASSERT_EQ_UINT(materialized.ndecls, 1);
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
    char *payload;

    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_BODY_SUMMARY);
    ASSERT_NOT_NULL(payload);
    wrong_phase = xg_global_evidence_cache_key(&ev, XG_EVIDENCE_CACHE_DECLARATIONS);
    ASSERT(!xg_evidence_cache_payload_matches(payload, &wrong_phase));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST(cache_payload_materialize_rejects_global_count_only_payload) {
    XgGlobalEvidence ev = {0};
    XgGlobalEvidence materialized = {0};
    char *payload;

    add_sample_body_summary(&ev);
    payload = xg_global_evidence_cache_payload_dump(&ev, XG_EVIDENCE_CACHE_GLOBAL_EVIDENCE);
    ASSERT_NOT_NULL(payload);
    ASSERT(!xg_evidence_cache_payload_materialize(payload, &materialized));

    xr_free(payload);
    xg_global_evidence_free(&ev);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Global Evidence Cache Payload");
RUN_TEST(cache_payload_parse_exposes_validated_body);
RUN_TEST(cache_payload_materializes_declaration_summary);
RUN_TEST(cache_payload_materializes_semantic_graph_summary);
RUN_TEST(cache_payload_parse_rejects_body_drift);
RUN_TEST(cache_payload_matches_rejects_wrong_phase_key);
RUN_TEST(cache_payload_materialize_rejects_global_count_only_payload);

TEST_MAIN_END()
