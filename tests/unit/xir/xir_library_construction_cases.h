/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_construction_cases.h - Source construction permission checks
 *
 * KEY CONCEPT:
 *   Source programs and complete historical packets establish fixed expectations.
 *   Construction authority is validated with its owning module.
 */
#ifndef XIR_LIBRARY_CONSTRUCTION_CASES_H
#define XIR_LIBRARY_CONSTRUCTION_CASES_H
#include "xir_library_typed_programs.h"
#include "xir_checked_scalar71_golden.h"
#include "xir_checked_scalar65_golden.h"
static void library_construction_old_packet(const void *bytes,size_t length) {
    size_t attempts=source_attempts,blocks=source_live,live_bytes=source_bytes;
    XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_checked_read(library_context,bytes,length,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output && source_attempts==attempts && source_live==blocks && source_bytes==live_bytes);
    output=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(library_context,bytes,length,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==(XrXirArtifact *)(uintptr_t)1 && source_attempts==attempts &&
        source_live==blocks && source_bytes==live_bytes);
}
static void library_construction_reject(const XrXirModule *module,const XrXirConstruction *construction) {
    size_t blocks=source_live,bytes=source_bytes;
    XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_recheck_v2(library_context,module,construction,&output,NULL)!=XR_XIR_OK);
    CHECK(!output && source_live==blocks && source_bytes==bytes);
    output=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_recheck_v2(library_context,module,construction,&output,NULL)!=XR_XIR_OK);
    CHECK(output==(XrXirArtifact *)(uintptr_t)1 && source_live==blocks && source_bytes==bytes);
}
static void library_construction_reject_rows(const XrXirModule *module,
    const XrXirConstructionRow *rows,uint32_t count) {
    size_t blocks=source_live,bytes=source_bytes;
    XrXirConstruction *bad=NULL;
    CHECK(xr_xir_compile_construction_new(library_context,module->types,rows,count,&bad)==XR_XIR_OK && bad);
    library_construction_reject(module,bad);
    xr_xir_compile_construction_free(bad);
    CHECK(source_live==blocks && source_bytes==bytes);
}
static XrXirConstructionRow *library_construction_rows(const XrXirConstruction *construction) {
    uint32_t count=xr_xir_compile_construction_count(construction);
    CHECK(construction && count);
    XrXirConstructionRow *rows=malloc(count*sizeof(*rows));CHECK(rows);
    for(uint32_t n=0;n<count;++n){
        const XrXirConstructionRow *row=xr_xir_compile_construction_row(construction,n);CHECK(row);
        rows[n]=*row;
    }
    return rows;
}
static void library_construction_attacks(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(artifact);
    const XrXirNominalTable *original=module->types->nominals;
    CHECK(original && original->declarations && construction);
    uint32_t owner=UINT32_MAX,field=UINT32_MAX;
    for(uint32_t n=0;n<original->count;++n)
        if(original->declarations[n].name.length==4 &&
            !memcmp(original->declarations[n].name.bytes,"Auto",4)) owner=n;
    CHECK(owner!=UINT32_MAX);
    const XrXirConstructionRow *nominal=xr_xir_compile_construction_row(construction,owner);
    CHECK(nominal && nominal->default_initializer);
    for(uint32_t f=0;f<nominal->field_count;++f)
        if(nominal->field_initializers[f]){field=f;break;}
    CHECK(field!=UINT32_MAX);
    XrXirConstructionRow *rows=library_construction_rows(construction);
    uint32_t count=xr_xir_compile_construction_count(construction);
    uint32_t *fields=malloc(nominal->field_count*sizeof(*fields));CHECK(fields);
    memcpy(fields,nominal->field_initializers,nominal->field_count*sizeof(*fields));
    rows[owner].field_initializers=fields;
    uint32_t saved=fields[field];
    fields[field]=module->function_count+1;
    library_construction_reject_rows(module,rows,count);
    fields[field]=nominal->default_initializer;
    library_construction_reject_rows(module,rows,count);
    fields[field]=saved;rows[owner].default_initializer=saved;
    library_construction_reject_rows(module,rows,count);
    rows[owner].default_initializer=module->function_count+1;
    library_construction_reject_rows(module,rows,count);
    rows[owner].default_initializer=nominal->default_initializer;
    if(nominal->field_count>1){
        uint32_t other=field?0:1,old=fields[other];
        fields[other]=saved;library_construction_reject_rows(module,rows,count);fields[other]=old;
    }
    library_construction_reject(module,NULL);
    size_t blocks=source_live,bytes=source_bytes;
    XrXirConstruction *bad=NULL;
    CHECK(xr_xir_compile_construction_new(library_context,module->types,rows,count-1,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
    rows[owner].field_count=nominal->field_count-1;
    CHECK(xr_xir_compile_construction_new(library_context,module->types,rows,count,&bad)==XR_XIR_BAD_STRUCTURE&&!bad);
    bad=(XrXirConstruction *)(uintptr_t)1;
    CHECK(xr_xir_compile_construction_new(library_context,module->types,rows,count,&bad)==XR_XIR_BAD_STRUCTURE);
    CHECK(bad==(XrXirConstruction *)(uintptr_t)1 && source_live==blocks && source_bytes==bytes);
    free(fields);free(rows);
}
static void library_construction_projection(const XrXirArtifact *checked) {
    XrXirArtifact *specialized=NULL;
    CHECK(xr_xir_compile_specialize(checked,&specialized,NULL)==XR_XIR_OK);
    const XrXirModule *m=xr_xir_compile_artifact_module(specialized);
    CHECK(xir_effect_evidence_is_instance(m) && m->provenance->source);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(specialized);
    const XrXirNominalTable *table=m->types->nominals;
    CHECK(construction && xr_xir_compile_construction_count(construction)==table->count);
    for(uint32_t n=0;n<table->count;++n){
        const XrXirConstructionRow *row=xr_xir_compile_construction_row(construction,n);
        CHECK(row && !row->default_initializer && row->field_count==table->declarations[n].field_count);
        for(uint32_t f=0;f<row->field_count;++f)CHECK(!row->field_initializers[f]);
    }
    library_construction_attacks(m->provenance->source);
    XrXirConstructionRow *rows=library_construction_rows(construction);
    rows[0].default_initializer=1;library_construction_reject_rows(m,rows,table->count);
    rows[0].default_initializer=0;
    uint32_t owner=UINT32_MAX;
    for(uint32_t n=0;n<table->count;++n)if(rows[n].field_count){owner=n;break;}
    CHECK(owner!=UINT32_MAX);
    uint32_t *fields=calloc(rows[owner].field_count,sizeof(*fields));CHECK(fields);
    rows[owner].field_initializers=fields;fields[0]=1;
    library_construction_reject_rows(m,rows,table->count);
    free(fields);free(rows);xr_xir_compile_artifact_free(specialized);
}

static void library_construction_case(void) {
    (void)checked_scalar71_digest;
    library_construction_old_packet(checked_scalar71_golden,sizeof(checked_scalar71_golden));
    library_construction_old_packet(checked_scalar65_golden,sizeof(checked_scalar65_golden));
    conflict_text("construction.xr",library_construction_definition);
    conflict_text("construction_receiver.xr",library_construction_consumer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"construction.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/construction.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirArtifact *decoded=NULL;
    CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    library_construction_attacks(decoded);
    xr_xir_compile_artifact_free(decoded);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(input.packet,input.length,input.sha256);
    CHECK(!remove(XR_SOURCE_FIXTURES "/construction.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/construction_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Owned construction metadata Source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_typed_closed_negative(catalog,authority,"import \"./construction\" as lib;export fn result()->i64{const r=lib.Required();return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./construction\" as lib;export fn result()->i64{const r=lib.Required{n:41};return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./construction\" as lib;export fn result()->i64{const r=lib.Locked{n:41};return 41;}",XR_XIR_BAD_TYPE);
    /* Explicit zero-argument construction is not implicit field-default permission. */
    library_typed_closed_negative(catalog,authority,"import \"./construction\" as lib;struct Holder{const value:lib.Explicit;}export fn result()->i64{const h=Holder();return h.value.n;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./construction\" as lib;export fn result()->i64{return lib.PrivateCtor().n;}",XR_XIR_BAD_TYPE);
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/construction_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"construction source %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    library_construction_attacks(result.checked);
    library_construction_projection(result.checked);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/construction_receiver.xr"));
    puts("construction initializer owned metadata literal41 owned-bytes5 realSource twoInstances physical0 PASS");
}
#endif // XIR_LIBRARY_CONSTRUCTION_CASES_H
