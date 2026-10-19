/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_nominal_cases.h - Governed enum metadata ownership and atomic publication
 *
 * KEY CONCEPT:
 *   Borrowed producers can die before checked replay and runtime metadata use.
 */
#ifndef XIR_ATOMIC_NOMINAL_CASES_H
#define XIR_ATOMIC_NOMINAL_CASES_H
#include "xir_construction_fixture.h"
#include "module/xmodule_identity.h"
#include "xir/xxir_type_arena.h"
#include "base/xsha256.h"
#include "xir_generic_method_golden.h"
#include "xir_ordering25_golden.h"
#include "xir_ordering65_golden.h"
#include "xir_ordering72_golden.h"
static const char *const ordering_variant_names[] = {"Relaxed","Acquire","Release","AcquireRelease","SeqCst"};
static XrXirStatus ordering_factory(const XrXirCompileContext *context, char **name) {
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"prelude",NULL};
    XrModuleStatus status=xr_compile_module_identity_from_logical(context->resources,&authority,
        "prelude/builtin_symbols.def",name);
    return status==XR_MODULE_OK?XR_XIR_OK:status==XR_MODULE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        status==XR_MODULE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
}
static XrXirNominalDeclaration ordering_declaration(char *module, XrXirNominalVariant variants[5]) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    CHECK(native && native->kind==XR_NATIVE_DECLARATION_VALUE);
    for(unsigned i=0;i<5;++i)variants[i]=(XrXirNominalVariant){
        {ordering_variant_names[i],(uint32_t)strlen(ordering_variant_names[i])},0,0};
    XrXirNominalDeclaration declaration={.module={module,(uint32_t)strlen(module)},
        .name={"Ordering",8},.exported=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=5};
    declaration.native.native_id=XR_NATIVE_DECLARATION_ORDERING;
    memcpy(declaration.native.source_fingerprint,native->source_fingerprint.bytes,32);
    return declaration;
}
static XrXirStatus ordering_projection_pipeline(const XrXirCompileContext *context,void *unused) {
    (void)unused;char *name=NULL;XrXirStatus status=ordering_factory(context,&name);
    XrXirNominalTable *copy=NULL,*projected=NULL;XrXirTypeArena *arena=NULL;
    XrXirNominalVariant variants[5];XrXirNominalDeclaration d={0};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}};
    XrXirNominalTable borrowed={.declarations=&d,.count=1};XrXirTypes types={&node,1,&borrowed,NULL};
    if(status==XR_XIR_OK){d=ordering_declaration(name,variants);
        status=xr_xir_compile_nominal_clone(context,&borrowed,&types,&copy);if(status!=XR_XIR_OK)CHECK(!copy);}
    xr_compile_resources_free(name);name=NULL;
    if(status==XR_XIR_OK){CHECK(copy->declarations[0].module.bytes!=d.module.bytes);
        status=xr_xir_compile_nominal_project(context,copy,&projected);if(status!=XR_XIR_OK)CHECK(!projected);}
    xr_xir_compile_nominal_free(copy);copy=NULL;
    if(status==XR_XIR_OK){types.nominals=projected;XrXirValueStatus value_status=xr_xir_compile_type_arena_new(context,&types,&arena);
        status=value_status==XR_XIR_VALUE_OK?XR_XIR_OK:value_status==XR_XIR_VALUE_OOM?XR_XIR_OUT_OF_MEMORY:
            value_status==XR_XIR_VALUE_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_TYPE;
        if(status!=XR_XIR_OK)CHECK(!arena);}
    xr_xir_compile_nominal_free(projected);projected=NULL;
    if(status==XR_XIR_OK){const XrXirTypes *owned=xr_xir_compile_type_arena_types(arena);
        CHECK(xr_xir_nominal_native_ordering(owned,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE));
        const XrXirNominalNativeRecord *record=xr_xir_nominal_native_record(owned,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE);
        CHECK(record && record->native_id==4 && !memcmp(record->source_fingerprint,d.native.source_fingerprint,32));
        CHECK(!xr_xir_nominal_native_record(owned,XR_XIR_I64));}
    xr_xir_compile_type_arena_drop(arena);return status;
}
static XrXirStatus ordering_checked_pipeline(const XrXirCompileContext *context,void *unused) {
    (void)unused;char *name=NULL;XrXirStatus status=ordering_factory(context,&name);
    XrXirNominalVariant variants[5];XrXirNominalDeclaration d={0};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}};
    XrXirNominalTable table={.declarations=&d,.count=1};XrXirTypes types={&node,1,&table,NULL};
    XrXirInstruction unit={.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    XrXirInstruction main_ops[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}}};
    XrXirBlock unit_block={.first=0,.count=1},main_block={.first=0,.count=2};XrXirFunction functions[3]={
        {.name="root_init",.name_length=9,.blocks=&unit_block,.block_count=1,.instructions=&unit,.instruction_count=1,.result=XR_XIR_UNIT},
        {.name="ordering_init",.name_length=13,.blocks=&unit_block,.block_count=1,.instructions=&unit,.instruction_count=1,.result=XR_XIR_UNIT},
        {.name="main",.name_length=4,.blocks=&main_block,.block_count=1,.instructions=main_ops,.instruction_count=2,.result=XR_XIR_I64}};
    uint32_t dependency=1;XrXirSourceModule modules[2]={{"root",4,&dependency,1,0},{name,0,NULL,0,1}};
    XrXirFunctionIdentity identities[3]={{.module=0},{.module=1},{.module=0,.exported=1}};
    XrXirDeclarations declarations={.modules=modules,.module_count=2,.functions=identities,.root_module=0,.entry_function=2};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,.declarations=&declarations,
        .types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*read=NULL,*specialized=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    if(status==XR_XIR_OK){d=ordering_declaration(name,variants);modules[1].name_length=d.module.length;
        status=xir_fixture_check(context, &module, &checked, NULL);if(status!=XR_XIR_OK)CHECK(!checked);}
    xr_compile_resources_free(name);name=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_write(checked,&packet,NULL);if(status!=XR_XIR_OK)CHECK(!packet.bytes && !packet.length);}
    xr_xir_compile_artifact_free(checked);checked=NULL;
    if(status==XR_XIR_OK){CHECK(packet.length==sizeof(ordering72_golden) &&
            !memcmp(packet.bytes,ordering72_golden,sizeof(ordering72_golden)));
        status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    if(status==XR_XIR_OK){status=xr_xir_compile_specialize(read,&specialized,NULL);if(status!=XR_XIR_OK)CHECK(!specialized);}
    xr_xir_compile_artifact_free(read);read=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    xr_xir_compile_artifact_free(specialized);specialized=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_artifact_verify(lowered,NULL);
        if(status==XR_XIR_OK)CHECK(xr_xir_nominal_native_ordering(xr_xir_compile_artifact_module(lowered)->types,
            (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE));}
    if(status!=XR_XIR_OK && source_program_compile_fail_at==SIZE_MAX)fprintf(stderr,"Ordering pipeline status %u\n",status);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&packet);return status;
}
static void ordering_negative_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    char *name=NULL;CHECK(ordering_factory(&owner.context,&name)==XR_XIR_OK);
    XrXirNominalVariant variants[5];XrXirNominalDeclaration valid=ordering_declaration(name,variants);
    XrXirNominalTable table={.declarations=&valid,.count=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}};
    XrXirTypes types={&node,1,&table,NULL};
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    for(unsigned mutation=0;mutation<11;++mutation){XrXirNominalDeclaration bad=valid;
        switch(mutation){case 0:bad.native.native_id=0;break;case 1:bad.native.native_id=3;break;
        case 2:bad.native.source_fingerprint[0]^=1;break;case 3:bad.exported=0;break;
        case 4:bad.flags=1;break;case 5:bad.kind=XR_XIR_NOMINAL_CLASS;break;
        case 6:bad.name=(XrXirLiteral){"Other",5};break;case 7:bad.module=(XrXirLiteral){"xray-native:prelude/Ordering",27};break;
        case 8:bad.variant_count=4;break;case 9:variants[0].name=(XrXirLiteral){"Wrong",5};break;
        default:variants[0].field_begin=1;break;}
        table.declarations=&bad;XrXirNominalTable *empty=NULL;
        CHECK(xr_xir_compile_nominal_clone(&owner.context,&table,&types,&empty)==XR_XIR_BAD_STRUCTURE && !empty);
        valid=ordering_declaration(name,variants);table.declarations=&valid;
    }
    valid.native=(XrXirNominalNativeRecord){0};
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    CHECK(!xr_xir_nominal_native_ordering(&types,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE));
    xr_compile_resources_free(name);library_compile_owner_drop(&owner);
}
#include "xir_atomic_nominal_packet_cases.h"
static void atomic_nominal_cases(void) {
    ordering_negative_cases();
    ordering_packet_cases();ordering_float_constant_cases();
    library_compile_operation_cases("Ordering clone/project/arena",ordering_projection_pipeline,NULL);
    library_compile_operation_cases("Ordering checked/codec/specialize/lower",ordering_checked_pipeline,NULL);
}
#endif // XIR_ATOMIC_NOMINAL_CASES_H
