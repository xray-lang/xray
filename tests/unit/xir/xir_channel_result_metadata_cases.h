/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_result_metadata_cases.h - Native result identity rejection controls
 */
#ifndef XIR_CHANNEL_RESULT_METADATA_CASES_H
#define XIR_CHANNEL_RESULT_METADATA_CASES_H
typedef struct ResultMetadataFixture {
    XrXirConstraint constraint;
    XrXirNominalField field;
    XrXirNominalVariant recv[4],send[4];
    XrXirNominalDeclaration declarations[2];
    XrXirNominalTable table;
    XrXirType arguments[2];
    XrXirTypeNode nodes[3];
    XrXirTypes types;
} ResultMetadataFixture;
static void result_metadata_init(ResultMetadataFixture *fixture) {
    *fixture=(ResultMetadataFixture){0};
    fixture->field=(XrXirNominalField){{"value",5},(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,0};
    const char *recv[]={"Value","Empty","Timeout","Closed"};
    const char *send[]={"Sent","Full","Timeout","Closed"};
    for (unsigned i=0;i<4;++i) {
        fixture->recv[i]=(XrXirNominalVariant){{recv[i],(uint32_t)strlen(recv[i])},i ? 1u : 0u,i ? 0u : 1u};
        fixture->send[i]=(XrXirNominalVariant){{send[i],(uint32_t)strlen(send[i])},0,0};
    }
    const char *module="stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def";
    fixture->declarations[0]=(XrXirNominalDeclaration){.module={module,(uint32_t)strlen(module)},.name={"Recv",4},
        .exported=1,.constraints=&fixture->constraint,.parameter_count=1,.fields=&fixture->field,.field_count=1,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=fixture->recv,.variant_count=4,.native={.native_id=XR_NATIVE_DECLARATION_RECV}};
    fixture->declarations[1]=(XrXirNominalDeclaration){.module={module,(uint32_t)strlen(module)},.name={"SendResult",10},
        .exported=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=fixture->send,.variant_count=4,
        .native={.native_id=XR_NATIVE_DECLARATION_SEND_RESULT}};
    for (unsigned i=0;i<2;++i) {
        const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(fixture->declarations[i].native.native_id);
        CHECK(native && xr_native_declaration_validate(native));
        memcpy(fixture->declarations[i].native.source_fingerprint,native->source_fingerprint.bytes,32);
    }
    fixture->table=(XrXirNominalTable){fixture->declarations,2,NULL};
    fixture->arguments[0]=XR_XIR_I64; fixture->arguments[1]=XR_XIR_UNIT;
    for (unsigned i=0;i<2;++i) fixture->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={0,&fixture->arguments[i],1,&fixture->arguments[i],1}};
    fixture->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1}};
    fixture->types=(XrXirTypes){fixture->nodes,3,&fixture->table,NULL};
}
static void result_metadata_rejected(ResultOwner *owner,const XrXirTypes *types,unsigned axis) {
    size_t live=compiler_memory.live,bytes=compiler_memory.bytes;
    XrXirStatus status=xr_xir_compile_types_structure_verify(&owner->context,types);
    if (status==XR_XIR_OK) fprintf(stderr,"Result metadata accepted axis=%u\n",axis);
    CHECK(status==XR_XIR_BAD_STRUCTURE || status==XR_XIR_BAD_TYPE);
    CHECK(compiler_memory.live==live && compiler_memory.bytes==bytes);
}
static void result_raw_metadata(void) {
    ResultOwner owner={0}; result_owner_ready(&owner); ResultMetadataFixture fixture;
    result_metadata_init(&fixture);
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&fixture.types)==XR_XIR_OK);
    for (unsigned axis=0;axis<20;++axis) {
        result_metadata_init(&fixture); XrXirNominalDeclaration *recv=&fixture.declarations[0];
        switch (axis) {
        case 0: recv->native.native_id=UINT32_MAX; break;
        case 1: recv->native.source_fingerprint[0]^=1; break;
        case 2: recv->module=(XrXirLiteral){"foreign",7}; break;
        case 3: recv->name=(XrXirLiteral){"Lookalike",9}; break;
        case 4: recv->exported=0; break;
        case 5: recv->kind=XR_XIR_NOMINAL_STRUCT; break;
        case 6: recv->parameter_count=0; recv->constraints=NULL; break;
        case 7: recv->field_count=0; recv->fields=NULL; break;
        case 8: fixture.field.name=(XrXirLiteral){"other",5}; break;
        case 9: fixture.field.type=XR_XIR_I64; break;
        case 10: fixture.constraint.markers=XR_XIR_CONSTRAINT_SENDABLE; break;
        case 11: fixture.field.flags=XR_XIR_FIELD_MUTABLE; break;
        case 12: fixture.recv[0].name=(XrXirLiteral){"Empty",5}; fixture.recv[1].name=(XrXirLiteral){"Value",5}; break;
        case 13: fixture.recv[0].field_count=0; fixture.recv[1].field_begin=0; fixture.recv[1].field_count=1; break;
        case 14: recv->flags=XR_XIR_NOMINAL_MODULE_LITERAL; break;
        case 15: fixture.declarations[1].native.source_fingerprint[31]^=1; break;
        case 16: fixture.declarations[1].native.native_id=XR_NATIVE_DECLARATION_RECV; break;
        case 17: fixture.send[0].name=(XrXirLiteral){"Full",4}; fixture.send[1].name=(XrXirLiteral){"Sent",4}; break;
        case 18: recv->native.native_id=0; memset(recv->native.source_fingerprint,0,32); break;
        case 19: fixture.arguments[0]=XR_XIR_UNIT; fixture.field.type=XR_XIR_UNIT; break;
        }
        result_metadata_rejected(&owner,&fixture.types,axis);
    }
    result_owner_free(&owner);
}
static void result_projected_metadata(void) {
    ResultOwner owner={0}; result_owner_ready(&owner);
    const XrXirTypes *owned=xr_xir_compile_type_arena_types(owner.arena);
    CHECK(owned->count==19 && owned->nominals->count==2);
    for (unsigned axis=0;axis<16;++axis) {
        XrXirTypeNode nodes[19]; memcpy(nodes,owned->nodes,sizeof(nodes));
        XrXirNominalIdentity identities[2]; memcpy(identities,owned->nominals->identities,sizeof(identities));
        XrXirNominalFieldIdentity field=identities[0].fields[0]; identities[0].fields=&field;
        XrXirNominalVariant variants[4]; memcpy(variants,identities[0].variants,sizeof(variants)); identities[0].variants=variants;
        XrXirNominalTable table={NULL,2,identities}; XrXirTypes types={nodes,19,&table,NULL};
        XrXirType wrong=XR_XIR_STRING;
        switch (axis) {
        case 0: identities[0].native.source_fingerprint[0]^=1; break;
        case 1: identities[0].native.native_id=XR_NATIVE_DECLARATION_SEND_RESULT; break;
        case 2: identities[0].module=(XrXirLiteral){"foreign",7}; break;
        case 3: identities[0].name=(XrXirLiteral){"Other",5}; break;
        case 4: identities[0].exported=0; break;
        case 5: identities[0].arity=0; break;
        case 6: identities[0].kind=XR_XIR_NOMINAL_STRUCT; break;
        case 7: field.name=(XrXirLiteral){"other",5}; break;
        case 8: field.flags=XR_XIR_FIELD_PRIVATE; break;
        case 9: variants[0].field_count=0; variants[1].field_begin=0; variants[1].field_count=1; break;
        case 10: nodes[RESULT_I64-256].nominal.fields=&wrong; break;
        case 11: nodes[RESULT_I64-256].nominal.arguments=&wrong; break;
        case 12: identities[1].native.source_fingerprint[31]^=1; break;
        case 13: identities[1].arity=1; break;
        case 14: nodes[RESULT_UNIT-256].nominal.declaration=1; break;
        case 15: nodes[RESULT_I64-256].nominal.field_count=0; nodes[RESULT_I64-256].nominal.fields=NULL; break;
        }
        result_metadata_rejected(&owner,&types,20+axis);
    }
    result_owner_free(&owner);
    puts("Channel result identity: 20 raw and 16 projected rejection axes physical=0");
}
#endif // XIR_CHANNEL_RESULT_METADATA_CASES_H
