/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_lowered_snapshot_cases.h - Exact physical copy controls, no permissions
 */
typedef struct LoweredSnapshotFixture {
    XrXirNominalFieldIdentity field;
    XrXirNominalVariant variant;
    XrXirNominalIdentity identity;
    XrXirNominalTable table;
    XrXirType payload, argument;
    XrXirCallableParameter parameter;
    XrXirTypeNode nodes[3];
    XrXirTypes types;
} LoweredSnapshotFixture;

static void lowered_snapshot_fixture(LoweredSnapshotFixture *f) {
    *f = (LoweredSnapshotFixture){0};
    f->field = (XrXirNominalFieldIdentity){.name = {"payload", 7}};
    f->variant = (XrXirNominalVariant){.name = {"Value", 5}, .field_count = 1};
    f->identity = (XrXirNominalIdentity){.module = {"snapshot", 8}, .name = {"Item", 4},
        .exported = 1, .arity = 1, .fields = &f->field, .field_count = 1, .kind = XR_XIR_NOMINAL_ENUM,
        .variants = &f->variant, .variant_count = 1};
    f->table = (XrXirNominalTable){.count = 1, .identities = &f->identity};
    f->payload = f->argument = XR_XIR_I64;
    f->parameter = (XrXirCallableParameter){(XrXirType)257, XR_PARAM_REF};
    f->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL,
        .nominal = {.arguments = &f->argument, .argument_count = 1,
            .fields = &f->payload, .field_count = 1}};
    f->nodes[1] = (XrXirTypeNode){.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)256};
    f->nodes[2] = (XrXirTypeNode){.kind = XR_XIR_TYPE_CALLABLE, .parameters = &f->parameter,
        .parameter_count = 1, .result = XR_XIR_I64,
        .flags = XR_XIR_CALLABLE_ROOT_NONE | XR_XIR_CALLABLE_NO_SUSPEND};
    f->types = (XrXirTypes){.nodes = f->nodes, .count = 3, .nominals = &f->table};
}

/* All records are temporary comparisons. No fake construction, invocation or
 * permission is created, and every live owner is restored before release. */
static void lowered_snapshot_rejections(const XrXirCompileContext *context,
    const XrXirTypes *owned, LoweredSnapshotFixture *f) {
#define LOWERED_SNAPSHOT_REJECT(change, restore) do { \
    change; CHECK(effect_lowered_snapshot_match(context, owned, &f->types) == XR_XIR_BAD_STRUCTURE); \
    restore; } while (0)
    LOWERED_SNAPSHOT_REJECT(f->types.count = 2, f->types.count = 3);
    LOWERED_SNAPSHOT_REJECT(f->types.nodes = NULL, f->types.nodes = f->nodes);
    LOWERED_SNAPSHOT_REJECT(f->types.nominals = NULL, f->types.nominals = &f->table);
    XrXirInterfaceTable interfaces = {0};
    LOWERED_SNAPSHOT_REJECT(f->types.interfaces = &interfaces, f->types.interfaces = NULL);
    LOWERED_SNAPSHOT_REJECT(f->table.count = 2, f->table.count = 1);
    LOWERED_SNAPSHOT_REJECT(f->table.identities = NULL, f->table.identities = &f->identity);
    XrXirNominalDeclaration declaration = {0};
    LOWERED_SNAPSHOT_REJECT(f->table.declarations = &declaration, f->table.declarations = NULL);
    LOWERED_SNAPSHOT_REJECT(f->identity.exported = 0, f->identity.exported = 1);
    LOWERED_SNAPSHOT_REJECT(f->identity.arity = 0, f->identity.arity = 1);
    LOWERED_SNAPSHOT_REJECT(f->identity.kind = XR_XIR_NOMINAL_STRUCT, f->identity.kind = XR_XIR_NOMINAL_ENUM);
    LOWERED_SNAPSHOT_REJECT(f->identity.flags = 1, f->identity.flags = 0);
    LOWERED_SNAPSHOT_REJECT(f->identity.field_count = 0, f->identity.field_count = 1);
    LOWERED_SNAPSHOT_REJECT(f->identity.variant_count = 0, f->identity.variant_count = 1);
    LOWERED_SNAPSHOT_REJECT(f->identity.fields = NULL, f->identity.fields = &f->field);
    LOWERED_SNAPSHOT_REJECT(f->identity.variants = NULL, f->identity.variants = &f->variant);
    LOWERED_SNAPSHOT_REJECT(f->identity.native.native_id = 1, f->identity.native.native_id = 0);
    for (uint32_t b = 0; b < 32; ++b)
        LOWERED_SNAPSHOT_REJECT(f->identity.native.source_fingerprint[b] = 1,
            f->identity.native.source_fingerprint[b] = 0);
    LOWERED_SNAPSHOT_REJECT(f->identity.module.bytes = "snapshox", f->identity.module.bytes = "snapshot");
    LOWERED_SNAPSHOT_REJECT(f->identity.module.bytes = NULL, f->identity.module.bytes = "snapshot");
    LOWERED_SNAPSHOT_REJECT(f->identity.name.bytes = "Itez", f->identity.name.bytes = "Item");
    LOWERED_SNAPSHOT_REJECT(f->identity.name.length = 3, f->identity.name.length = 4);
    LOWERED_SNAPSHOT_REJECT(f->field.name.bytes = "payloax", f->field.name.bytes = "payload");
    LOWERED_SNAPSHOT_REJECT(f->field.flags = 1, f->field.flags = 0);
    LOWERED_SNAPSHOT_REJECT(f->variant.name.bytes = "Valux", f->variant.name.bytes = "Value");
    LOWERED_SNAPSHOT_REJECT(f->variant.field_begin = 1, f->variant.field_begin = 0);
    LOWERED_SNAPSHOT_REJECT(f->variant.field_count = 0, f->variant.field_count = 1);
    LOWERED_SNAPSHOT_REJECT(f->nodes[1].kind = XR_XIR_TYPE_ARRAY, f->nodes[1].kind = XR_XIR_TYPE_CELL);
    LOWERED_SNAPSHOT_REJECT(f->nodes[1].element = XR_XIR_I64, f->nodes[1].element = (XrXirType)256);
    LOWERED_SNAPSHOT_REJECT(f->nodes[2].result = XR_XIR_BOOL, f->nodes[2].result = XR_XIR_I64);
    LOWERED_SNAPSHOT_REJECT(f->nodes[2].flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED,
        f->nodes[2].flags = XR_XIR_CALLABLE_ROOT_NONE | XR_XIR_CALLABLE_NO_SUSPEND);
    LOWERED_SNAPSHOT_REJECT(f->nodes[1].parameter_span = 1, f->nodes[1].parameter_span = 0);
    LOWERED_SNAPSHOT_REJECT(f->nodes[2].parameter_count = 0, f->nodes[2].parameter_count = 1);
    LOWERED_SNAPSHOT_REJECT(f->nodes[2].parameters = NULL, f->nodes[2].parameters = &f->parameter);
    LOWERED_SNAPSHOT_REJECT(f->parameter.mode = XR_PARAM_READ, f->parameter.mode = XR_PARAM_REF);
    LOWERED_SNAPSHOT_REJECT(f->parameter.type = XR_XIR_I64, f->parameter.type = (XrXirType)257);
    LOWERED_SNAPSHOT_REJECT(f->nodes[0].nominal.declaration = 1, f->nodes[0].nominal.declaration = 0);
    LOWERED_SNAPSHOT_REJECT(f->nodes[0].nominal.argument_count = 0, f->nodes[0].nominal.argument_count = 1);
    LOWERED_SNAPSHOT_REJECT(f->nodes[0].nominal.arguments = NULL, f->nodes[0].nominal.arguments = &f->argument);
    LOWERED_SNAPSHOT_REJECT(f->argument = XR_XIR_BOOL, f->argument = XR_XIR_I64);
    LOWERED_SNAPSHOT_REJECT(f->nodes[0].nominal.field_count = 0, f->nodes[0].nominal.field_count = 1);
    LOWERED_SNAPSHOT_REJECT(f->nodes[0].nominal.fields = NULL, f->nodes[0].nominal.fields = &f->payload);
    LOWERED_SNAPSHOT_REJECT(f->payload = XR_XIR_BOOL, f->payload = XR_XIR_I64);
    CHECK(effect_lowered_snapshot_match(context, owned, &f->types) == XR_XIR_OK);
#undef LOWERED_SNAPSHOT_REJECT
}

static XrXirStatus lowered_snapshot_empty_copy(const XrXirCompileContext *context, bool oracle) {
    XrXirTypes empty = {0};
    XrXirInstruction instruction = {.op = XR_XIR_RETURN};
    XrXirBlock block = {.count = 1};
    XrXirFunction function = {.name = "empty", .name_length = 5, .instructions = &instruction,
        .instruction_count = 1, .blocks = &block, .block_count = 1};
    XrXirModule module = {.stage = XR_XIR_LOWERED, .functions = &function,
        .function_count = 1, .types = &empty};
    EffectInvocationCertificate certificate = {.resources = context->resources,
        .terms = {.remaining = context}};
    XrXirStatus status = effect_invocation_copy_bodies(&certificate, &module);
    if (status == XR_XIR_OK) status = effect_lowered_snapshot_match(context, &certificate.terms.types, &empty);
    if (status == XR_XIR_OK && oracle) {
        CHECK(!certificate.lowered_types && !certificate.terms.types.count &&
            !certificate.terms.types.nodes && !certificate.terms.types.nominals &&
            !certificate.terms.types.interfaces && certificate.bodies.functions != &function &&
            certificate.bodies.functions[0].instructions != &instruction &&
            certificate.bodies.functions[0].blocks != &block &&
            certificate.bodies.functions[0].name != function.name);
        EffectInvocationCertificate rejected = {.resources = context->resources,
            .terms = {.remaining = context}};
        XrXirTypes missing = {.count = 1};module.types = &missing;
        CHECK(effect_invocation_copy_bodies(&rejected, &module) == XR_XIR_BAD_STRUCTURE);
        XrXirNominalTable malformed = {.count = 1};
        missing = (XrXirTypes){.nominals = &malformed};
        CHECK(effect_invocation_copy_bodies(&rejected, &module) == XR_XIR_BAD_STRUCTURE);
        CHECK(!rejected.lowered_types && !rejected.terms.types.nodes && !rejected.terms.memory);
        effect_terms_free(&rejected.terms);
    }
    effect_terms_free(&certificate.terms);
    xr_xir_compile_types_free(certificate.lowered_types);return status;
}

static XrXirStatus conditional_lowered_snapshot(const XrXirCompileContext *context,
    uint32_t mode, bool oracle) {
    if (mode >= 3) return XR_XIR_BAD_STRUCTURE;
    if (mode == 2) return lowered_snapshot_empty_copy(context, oracle);
    LoweredSnapshotFixture fixture;lowered_snapshot_fixture(&fixture);
    XrXirTypes *owned = NULL;
    XrXirStatus status = xr_xir_compile_types_structure_verify(context, &fixture.types);
    if (status == XR_XIR_OK) status = xr_xir_compile_types_clone(context, &fixture.types, &owned);
    if (status == XR_XIR_OK) {
        if (oracle) CHECK(owned && owned->nodes != fixture.nodes && owned->nominals != &fixture.table &&
            owned->nominals->identities != &fixture.identity &&
            owned->nodes[0].nominal.arguments != &fixture.argument &&
            owned->nodes[0].nominal.fields != &fixture.payload &&
            owned->nodes[2].parameters != &fixture.parameter &&
            owned->nominals->identities[0].fields != &fixture.field &&
            owned->nominals->identities[0].variants != &fixture.variant);
        if (mode) fixture.variant.field_begin = 1;
        status = effect_lowered_snapshot_match(context, owned, &fixture.types);
        if (mode && status == XR_XIR_BAD_STRUCTURE) status = XR_XIR_OK;
        else if (mode && status == XR_XIR_OK) status = XR_XIR_BAD_STRUCTURE;
        fixture.variant.field_begin = 0;
        if (status == XR_XIR_OK && oracle) {
            EffectTerms semantic = {.remaining = context, .types = *owned};
            CHECK(effect_terms_domain_match(&semantic, &fixture.types) == XR_XIR_BAD_TYPE);
            lowered_snapshot_rejections(context, owned, &fixture);
        }
    }
    xr_xir_compile_types_free(owned);return status;
}
