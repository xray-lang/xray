/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Canonical enum field ownership after source and catalog producers retire.
 */
#ifndef XIR_PRELUDE_ENDIAN_CASES_H
#define XIR_PRELUDE_ENDIAN_CASES_H
static XrXirSourceResult prelude_endian_check(const XrXirCompileContext *context,
    const XrXirLibraryCatalog *catalog,const char *file,bool text) {
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,file,&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status;
    if (text) {
        FILE *input=fopen(file,"rb");CHECK(input && !fseek(input,0,SEEK_END));
        long size=ftell(input);CHECK(size>0 && !fseek(input,0,SEEK_SET));
        char *bytes=xr_malloc((size_t)size+1);CHECK(bytes);
        CHECK(fread(bytes,1,(size_t)size,input)==(size_t)size && !fclose(input));bytes[size]=0;
        const char *name=strrchr(file,'/');CHECK(name);
        XrXirSourceText source={name+1,bytes,(size_t)size};
        status=xr_xir_compile_source_check_text(&request,&source,&result,&diagnostic,NULL);
        memset(bytes,0,(size_t)size);xr_free(bytes);
    } else status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    xr_compile_session_free(session);
    if (status!=XR_XIR_OK)
        fprintf(stderr,"Endian %s catalog%u text%u status%u %d:%d %s\n",file,catalog!=NULL,text,status,diagnostic.line,diagnostic.column,diagnostic.message);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(status==XR_XIR_OK && result.checked && view && view->complete);
    return result;
}
static void prelude_endian_refusal(const XrXirCompileContext *context,const XrXirLibraryCatalog *catalog) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/endian_refuse.xr",&authority,
        context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,catalog};
    char bytes[]="enum Endian { Local }\nexport fn consumerAnswer()->i64 { const order=Endian.LE;return 42; }\n";
    XrXirSourceText text={"endian_refuse.xr",bytes,sizeof(bytes)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL)==XR_XIR_BAD_TYPE);
    memset(bytes,0,sizeof(bytes));xr_compile_session_free(session);
    CHECK(!result.checked && !result.snapshot && strstr(diagnostic.message,"unknown enum variant or static method"));
    xr_xir_compile_source_result_free(&result);
}
static bool prelude_endian_literal(XrXirLiteral literal,const char *text) {
    return literal.length==strlen(text) && !memcmp(literal.bytes,text,literal.length);
}
static void prelude_endian_field(const XrXirArtifact *artifact,bool shadow) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    CHECK(module && module->types && module->types->nominals);
    const XrXirNominalTable *table=module->types->nominals;
    const XrXirNominalDeclaration *box=NULL;
    for (uint32_t i=0;i<table->count;++i)
        if (prelude_endian_literal(table->declarations[i].name,"EndianBox")) box=&table->declarations[i];
    CHECK(box && box->kind==XR_XIR_NOMINAL_CLASS && box->field_count==1 && box->fields);
    const XrXirTypeNode *type=xr_xir_type_node(module->types,box->fields[0].type);
    CHECK(type && type->kind==XR_XIR_TYPE_NOMINAL && type->nominal.declaration<table->count);
    const XrXirNominalDeclaration *enumeration=&table->declarations[type->nominal.declaration];
    CHECK(prelude_endian_literal(enumeration->name,"Endian") && enumeration->kind==XR_XIR_NOMINAL_ENUM);
    CHECK(!enumeration->native.native_id && !enumeration->parameter_count && !enumeration->field_count);
    if (shadow) {
        CHECK(enumeration->module.length==box->module.length &&
            !memcmp(enumeration->module.bytes,box->module.bytes,box->module.length));
        CHECK(enumeration->variant_count==1 && prelude_endian_literal(enumeration->variants[0].name,"Local"));
    } else {
        const char *names[]={"Native","LE","BE"};
        CHECK(prelude_endian_literal(enumeration->module,XIR_PRELUDE_CANONICAL) && enumeration->variant_count==3);
        for (uint32_t i=0;i<3;++i) CHECK(prelude_endian_literal(enumeration->variants[i].name,names[i]));
    }
}
static XrXirLibraryCatalog *prelude_endian_catalog(const XrXirCompileContext *context) {
    XrXirSourceResult producer={0};XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_prelude_library(context,&producer,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    char logical[]="prelude/builtin_symbols.def",space[]="prelude";char *physical=NULL;
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);
    CHECK(xr_realpath_owned(&policy,XR_SOURCE_STDLIB,&physical)==XR_OS_IO_OK);
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_STDLIB,space,physical},logical};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);memset(packet.bytes,0,packet.length);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    memset(logical,0,sizeof(logical));memset(space,0,sizeof(space));memset(physical,0,strlen(physical));policy.free(policy.context,physical);
    return catalog;
}
static char *prelude_endian_catalog_locator(const XrXirLibraryCatalog *catalog) {
    size_t count=0;const char *selected=NULL;
    const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    for (size_t i=0;i<count;++i) if (resources[i].canonical && !strcmp(resources[i].canonical,XIR_PRELUDE_CANONICAL)) {
        CHECK(!selected && resources[i].checked && resources[i].source_locator);selected=resources[i].source_locator;
    }
    CHECK(selected);size_t bytes=strlen(selected)+1;char *owned=xr_malloc(bytes);CHECK(owned);
    memcpy(owned,selected,bytes);return owned;
}
static void prelude_endian_snapshot_path(const XrXirSourceResult *result,const char *expected) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);CHECK(view && view->complete);
    uint32_t matches=0;
    for (uint32_t i=0;i<view->module_count;++i) if (!strcmp(view->modules[i].identity,XIR_PRELUDE_CANONICAL)) {
        CHECK(++matches==1);
        if (expected) CHECK(view->modules[i].path && !strcmp(view->modules[i].path,expected));
        else CHECK(!view->modules[i].path);
    }
    CHECK(matches==1);
}
static void prelude_endian_owner(void) {
    for (unsigned flow=0;flow<2;++flow) {
        LibraryCompileOwner compiler={0};
        CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
        const XrXirCompileContext *context=&compiler.context;
        XrXirLibraryCatalog *catalog=flow?prelude_endian_catalog(context):NULL;
        char *locator=flow?prelude_endian_catalog_locator(catalog):NULL;
        XrXirSourceResult accepted[3]={
            prelude_endian_check(context,catalog,XR_SOURCE_FIXTURES "/endian_field.xr",true),
            prelude_endian_check(context,catalog,XR_SOURCE_FIXTURES "/endian_field.xr",false),
            prelude_endian_check(context,catalog,XR_SOURCE_FIXTURES "/endian_shadow.xr",false)};
        prelude_endian_refusal(context,catalog);
        for (unsigned i=0;i<2;++i) prelude_endian_snapshot_path(&accepted[i],locator);
        xr_xir_compile_library_catalog_free(catalog);
        for (unsigned i=0;i<3;++i) {
            if (i<2) prelude_endian_snapshot_path(&accepted[i],locator);
            prelude_endian_field(accepted[i].checked,i==2);XrXirArtifact *owned=NULL;
            CHECK(prelude_roundtrip(context,accepted[i].checked,&owned)==XR_XIR_OK);
            xr_xir_compile_source_result_free(&accepted[i]);
            CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
            prelude_endian_field(owned,i==2);xr_xir_compile_artifact_free(owned);
        }
        xr_free(locator);library_compile_owner_drop(&compiler);
    }
    puts("Canonical Endian Source/File/Catalog fields, shadow and refusal after producer retirement PASS");
}
#endif // XIR_PRELUDE_ENDIAN_CASES_H
