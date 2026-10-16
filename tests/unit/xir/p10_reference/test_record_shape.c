/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_record_shape.c - Canonical names, source order and physical failure bounds
 */
#include "xir/xxir_record_shape.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);} } while (0)
#include "../xir_instance_compile_observer.h"
#include "../xir_source_fixture_owner.h"
static XrXirRecordField record_fields[]={{{"zeta",4},XR_XIR_I64},{{"alpha",5},XR_XIR_BOOL},
    {{"middle",6},XR_XIR_STRING},{{"\xe4\xb8\xad",3},XR_XIR_UNIT}};
static const char *record_sorted[]={"alpha","middle","zeta","\xe4\xb8\xad"};
static const XrXirType record_types[]={XR_XIR_BOOL,XR_XIR_STRING,XR_XIR_I64,XR_XIR_UNIT};
static void record_shape_expect(const XrXirRecordShape *shape,const XrXirRecordField *input,const XrXirType *expected) {
    CHECK(shape && shape->count==4 && shape->fields && shape->source_to_field);
    for (uint32_t i=0;i<4;++i) {
        CHECK(shape->fields[i].name.length==strlen(record_sorted[i]));
        CHECK(!memcmp(shape->fields[i].name.bytes,record_sorted[i],shape->fields[i].name.length));
        CHECK(shape->fields[i].type==expected[i] && shape->fields[i].name.bytes[shape->fields[i].name.length]==0);
        CHECK(shape->fields[i].name.bytes!=input[i].name.bytes);
        uint32_t at=shape->source_to_field[i];CHECK(at<4 && shape->fields[at].type==input[i].type);
        CHECK(shape->fields[at].name.length==input[i].name.length &&
            !memcmp(shape->fields[at].name.bytes,input[i].name.bytes,input[i].name.length));
    }
}
static void record_shape_permutations(SourceFixtureOwner *compiler) {
    unsigned cases=0;
    for (unsigned a=0;a<4;++a) for (unsigned b=0;b<4;++b) for (unsigned c=0;c<4;++c) for (unsigned d=0;d<4;++d) {
        if (a==b || a==c || a==d || b==c || b==d || c==d) continue;
        const XrXirRecordField fields[]={record_fields[a],record_fields[b],record_fields[c],record_fields[d]};
        XrXirRecordShapeInput input={NULL,fields,4,0};XrXirRecordShape *shape=NULL;
        CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK);
        record_shape_expect(shape,fields,record_types);xr_xir_compile_record_shape_free(shape);++cases;
    }
    CHECK(cases==24);
    char text[]="ephemeral";XrXirRecordField field={{text,9},XR_XIR_I64};
    XrXirRecordShapeInput input={NULL,&field,1,0};XrXirRecordShape *shape=NULL;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK);
    memset(text,'x',9);CHECK(!memcmp(shape->fields[0].name.bytes,"ephemeral",9));
    xr_compile_resources_release(compiler->context.resources);compiler->context.resources=NULL;
    CHECK(!memcmp(shape->fields[0].name.bytes,"ephemeral",9));xr_xir_compile_record_shape_free(shape);
    instance_compile_zero();source_fixture_owner_new(compiler);
}
static void record_shape_maximum(SourceFixtureOwner *compiler) {
    const uint32_t count=65536;void *memory=NULL,*names=NULL;
    CHECK(xr_compile_resources_calloc(compiler->context.resources,count,sizeof(XrXirRecordField),&memory)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(compiler->context.resources,count,7,&names)==XR_COMPILE_RESOURCE_OK);
    XrXirRecordField *fields=memory;char *text=names;
    for (uint32_t i=0;i<count;++i) {
        CHECK(snprintf(text+i*7,7,"f%05u",count-i-1)==6);
        fields[i]=(XrXirRecordField){{text+i*7,6},XR_XIR_I64};
    }
    XrXirRecordShapeInput input={NULL,fields,count,0};XrXirRecordShape *shape=NULL;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK && shape->count==count);
    for (uint32_t i=0;i<count;++i) {
        char expected[7];CHECK(snprintf(expected,sizeof(expected),"f%05u",i)==6);
        CHECK(!memcmp(shape->fields[i].name.bytes,expected,7) && shape->fields[i].type==XR_XIR_I64);
        CHECK(shape->source_to_field[count-i-1]==i);
    }
    xr_xir_compile_record_shape_free(shape);xr_compile_resources_free(names);xr_compile_resources_free(memory);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(compiler->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.peak_bytes<UINT64_C(8)*1024*1024 && stats.work<128000000);
    printf("Record shape65536 reverse fields peak=%llu work=%llu PASS\n",(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
}
static void record_shape_rejected(SourceFixtureOwner *compiler) {
    const char *bad[]={"", "1key", "has space", "a.b", "a-b", "_", "\x80", "\xc0\xaf", "a\n"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        XrXirRecordField field={{bad[i],(uint32_t)strlen(bad[i])},XR_XIR_I64};
        XrXirRecordShapeInput input={NULL,&field,1,0};XrXirRecordShape *shape=NULL;
        CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_STRUCTURE && !shape);
    }
    XrXirRecordField nul={{"a\0b",3},XR_XIR_I64};XrXirRecordShapeInput input={NULL,&nul,1,0};XrXirRecordShape *shape=NULL;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_STRUCTURE && !shape);
    XrXirRecordField duplicate[]={{{"same",4},XR_XIR_I64},{{"same",4},XR_XIR_STRING}};input.fields=duplicate;input.count=2;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_STRUCTURE && !shape);
    input.fields=&record_fields[0];input.count=1;shape=(XrXirRecordShape *)(uintptr_t)1;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_STRUCTURE && shape==(XrXirRecordShape *)(uintptr_t)1);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK && !memcmp(&before,&after,sizeof(before)));
    shape=NULL;input.count=0;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_STRUCTURE && !shape);
    input.fields=NULL;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK && shape && !shape->count &&
        !shape->fields && !shape->source_to_field);xr_xir_compile_record_shape_free(shape);
}
static void record_shape_generic(SourceFixtureOwner *compiler) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}};
    XrXirTypes pool={nodes,2,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(&compiler->context,&pool)==XR_XIR_OK);
    XrXirRecordField field={{"value",5},(XrXirType)256};XrXirRecordShapeInput input={&pool,&field,1,1};XrXirRecordShape *shape=NULL;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK && shape->fields[0].type==(XrXirType)256);
    xr_xir_compile_record_shape_free(shape);shape=NULL;input.parameter_count=0;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_TYPE && !shape);
    field.type=(XrXirType)257;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_TYPE && !shape);
    field.type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_BAD_TYPE && !shape);
    input.parameter_count=1;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK);xr_xir_compile_record_shape_free(shape);shape=NULL;
    XrCompileResourceStats before={0},failed={0},retried={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    field.type=(XrXirType)256;instance_compile_fail_at=instance_compile_attempts+1;instance_compile_injected=false;
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OUT_OF_MEMORY && !shape && instance_compile_injected);
    instance_compile_fail_at=SIZE_MAX;
    CHECK(xr_compile_resources_stats(compiler->context.resources,&failed)==XR_COMPILE_RESOURCE_OK);
    CHECK(failed.work>before.work && failed.allocated_bytes>before.allocated_bytes && failed.live_bytes==before.live_bytes);
    CHECK(xr_xir_compile_record_shape_new(&compiler->context,&input,&shape)==XR_XIR_OK);xr_xir_compile_record_shape_free(shape);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&retried)==XR_COMPILE_RESOURCE_OK);
    CHECK(retried.work>failed.work && retried.allocated_bytes>failed.allocated_bytes && retried.live_bytes==before.live_bytes);
}
static XrXirStatus record_shape_trial(XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompileResourceStatus opened=xr_compile_resources_new(&limits,&resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(opened==XR_COMPILE_RESOURCE_BUDGET || opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(!resources);instance_compile_zero();
        return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XrXirRecordShape *shape=NULL;
    XrXirTypeNode array={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};XrXirTypes types={&array,1,NULL,NULL};
    XrXirRecordField fields[4];memcpy(fields,record_fields,sizeof(fields));fields[0].type=(XrXirType)256;
    const XrXirType expected[]={XR_XIR_BOOL,XR_XIR_STRING,(XrXirType)256,XR_XIR_UNIT};
    XrXirRecordShapeInput input={&types,fields,4,0};
    XrXirStatus status=xr_xir_compile_types_structure_verify(&context,&types);
    if (status==XR_XIR_OK) status=xr_xir_compile_record_shape_new(&context,&input,&shape);
    if (status==XR_XIR_OK) record_shape_expect(shape,fields,expected);else CHECK(!shape);
    xr_xir_compile_record_shape_free(shape);
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK && stats->live_bytes==baseline.live_bytes);
    xr_compile_resources_release(resources);instance_compile_zero();return status;
}
static void record_shape_resources(void) {
    XrCompileResourceLimits limits={64*1024*1024,8*1024*1024,128000000};XrCompileResourceStats normal={0},observed={0};
    instance_compile_attempts=0;CHECK(record_shape_trial(limits,&normal)==XR_XIR_OK);size_t sites=instance_compile_attempts;
    CHECK(sites>1 && sites<20000);
    for (size_t site=0;site<sites;++site) {
        instance_compile_attempts=0;instance_compile_fail_at=site;instance_compile_injected=false;
        CHECK(record_shape_trial(limits,&observed)==XR_XIR_OUT_OF_MEMORY && instance_compile_injected);
    }
    instance_compile_fail_at=SIZE_MAX;
    XrCompileResourceLimits exact={normal.allocated_bytes,normal.peak_bytes,normal.work};
    CHECK(record_shape_trial(exact,&observed)==XR_XIR_OK);
    CHECK(observed.allocated_bytes==normal.allocated_bytes && observed.peak_bytes==normal.peak_bytes && observed.work==normal.work);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits short_limit=exact;
        if (!axis) --short_limit.allocated_bytes;else if (axis==1) --short_limit.live_bytes;else --short_limit.work;
        CHECK(record_shape_trial(short_limit,&observed)==XR_XIR_BUDGET);
    }
    for (uint64_t prefix=0;prefix<normal.work;++prefix) {
        XrCompileResourceLimits cut=limits;cut.work=prefix;
        CHECK(record_shape_trial(cut,&observed)==XR_XIR_BUDGET);
    }
    printf("Record shape all%zu allocator failures,three exact/minus1 axes,all work prefixes,physical0/0 work=%llu PASS\n",
        sites,(unsigned long long)normal.work);
}
int main(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    record_shape_permutations(&compiler);record_shape_rejected(&compiler);record_shape_generic(&compiler);
    record_shape_maximum(&compiler);
    source_fixture_owner_free(&compiler);instance_compile_zero();record_shape_resources();
    puts("Canonical record metadata24 permutations,owned names,source ordinals,generic scope and strict rejection PASS");return 0;
}
