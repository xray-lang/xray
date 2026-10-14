/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_type_description.c - Canonical families and ordered borrowed fields
 */
#include "xir/xxir_types.h"
#include "base/xcompile_resources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
static XrXirCompileContext context_new(uint64_t work) {
    XrXirCompileContext c={0};XrCompileResourceLimits caps={67108864,8388608,work};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats stats(XrXirCompileContext *c) {
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void budget_cases(const XrXirTypes *types) {
    for (unsigned limit=0;limit<2;++limit) {
        XrXirCompileContext c=context_new(1+limit);XrXirTypeDescription view={0};view.field_count=99;
        unsigned char saved[sizeof(view)];memcpy(saved,&view,sizeof(view));XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_type_description(&c,types,(XrXirType)256,&view)==(limit?XR_XIR_OK:XR_XIR_BUDGET));
        if (!limit) CHECK(!memcmp(saved,&view,sizeof(view)));
        XrCompileResourceStats after=stats(&c);
        CHECK(after.work==before.work+limit && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes && after.allocation_count==before.allocation_count);
        XrXirFieldDescription field={.flags=99},old=field;
        CHECK(xr_xir_compile_description_field(&c,&view,0,&field)==XR_XIR_BUDGET);
        CHECK(field.flags==old.flags && field.name.bytes==old.name.bytes && stats(&c).work==after.work);
        xr_compile_resources_release(c.resources);
    }
    for (unsigned limit=0;limit<2;++limit) {
        XrXirCompileContext c=context_new(2+limit);XrXirTypeDescription view={0};
        CHECK(xr_xir_compile_type_description(&c,types,(XrXirType)256,&view)==XR_XIR_OK);
        XrXirFieldDescription field={.name={"unchanged",9},.type=XR_XIR_BOOL,
            .template_type=XR_XIR_STRING,.flags=99,.resolved=false};
        unsigned char saved[sizeof(field)];memcpy(saved,&field,sizeof(field));
        XrCompileResourceStats before=stats(&c);
        CHECK(xr_xir_compile_description_field(&c,&view,0,&field)==(limit?XR_XIR_OK:XR_XIR_BUDGET));
        if (!limit) CHECK(!memcmp(saved,&field,sizeof(field)));
        else CHECK(field.resolved && field.type==XR_XIR_I8 && field.template_type==XR_XIR_I8 &&
            field.flags==XR_XIR_FIELD_PRIVATE && field.name.bytes[0]=='z');
        XrCompileResourceStats after=stats(&c);
        CHECK(after.work==before.work+limit && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes && after.allocation_count==before.allocation_count);
        memcpy(saved,&field,sizeof(field));
        CHECK(xr_xir_compile_description_field(&c,&view,1,&field)==XR_XIR_BUDGET);
        CHECK(!memcmp(saved,&field,sizeof(field)) && stats(&c).work==after.work);
        xr_compile_resources_release(c.resources);
    }
}
int main(void) {
    char names[2][2]={{'z',0},{'a',0}};
    XrXirNominalField fields[]={{{names[0],1},XR_XIR_I8,XR_XIR_FIELD_PRIVATE},
        {{names[1],1},XR_XIR_STRING,XR_XIR_FIELD_MUTABLE}};
    XrXirNominalDeclaration declaration={.module={"alpha",5},.name={"Record",6},.exported=1,
        .fields=fields,.field_count=2,.kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable table={&declaration,1,NULL};XrXirType member_types[]={XR_XIR_I8,XR_XIR_STRING};
    XrXirCallableParameter tuple_fields[]={{XR_XIR_I64,0},{XR_XIR_UNIT,0},{(XrXirType)257,0}};
    XrXirTypeNode nodes[8]={{0}};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.fields=member_types,.field_count=2}};
    nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256};
    nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=tuple_fields,.parameter_count=3};
    nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64};
    nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64};
    nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypes types={nodes,8,&table,NULL};XrXirCompileContext c=context_new(128000000);
    uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_types_structure_verify(&c,&types)==XR_XIR_OK);
    XrXirTypeDescription view={0};XrXirFieldDescription field={0};
    for (uint32_t i=0;i<8;++i) {
        CHECK(xr_xir_compile_type_description(&c,&types,(XrXirType)(256+i),&view)==XR_XIR_OK);
        CHECK(view.type==(XrXirType)(256+i) && view.node==&nodes[i] && view.kind==nodes[i].kind);
    }
    CHECK(xr_xir_compile_type_description(&c,NULL,XR_XIR_I64,&view)==XR_XIR_OK);
    CHECK(view.kind==0 && !view.node && view.type==XR_XIR_I64);
    XrXirTypeDescription scalar=view;
    CHECK(xr_xir_compile_type_description(&c,&types,(XrXirType)255,&view)==XR_XIR_BAD_TYPE);
    CHECK(view.type==scalar.type && view.node==scalar.node);
    CHECK(xr_xir_compile_type_description(&c,&types,(XrXirType)256,&view)==XR_XIR_OK);
    CHECK(view.nominal_kind==XR_XIR_NOMINAL_STRUCT && view.field_count==2 && !view.variant_count);
    CHECK(xr_xir_compile_description_field(&c,&view,0,&field)==XR_XIR_OK);
    CHECK(field.resolved && field.name.length==1 && field.name.bytes[0]=='z' && field.type==XR_XIR_I8 && field.flags==XR_XIR_FIELD_PRIVATE);
    CHECK(xr_xir_compile_description_field(&c,&view,1,&field)==XR_XIR_OK);
    CHECK(field.resolved && field.name.bytes[0]=='a' && field.type==XR_XIR_STRING && field.flags==XR_XIR_FIELD_MUTABLE);
    nodes[0].nominal.fields=NULL;nodes[0].nominal.field_count=0;
    CHECK(xr_xir_compile_types_structure_verify(&c,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_description_field(&c,&view,0,&field)==XR_XIR_OK);
    CHECK(!field.resolved && field.type==XR_XIR_UNIT && field.template_type==XR_XIR_I8 && field.name.bytes[0]=='z');
    nodes[0].nominal.fields=member_types;nodes[0].nominal.field_count=2;
    CHECK(xr_xir_compile_description_field(&c,&view,1,&field)==XR_XIR_OK && field.resolved);
    XrXirFieldDescription saved=field;
    CHECK(xr_xir_compile_description_field(&c,&view,2,&field)==XR_XIR_BAD_STRUCTURE);
    CHECK(field.name.bytes==saved.name.bytes && field.flags==saved.flags);
    CHECK(xr_xir_compile_type_description(&c,&types,(XrXirType)258,&view)==XR_XIR_OK);
    CHECK(view.field_count==3);
    CHECK(xr_xir_compile_description_field(&c,&view,1,&field)==XR_XIR_OK);
    CHECK(!field.name.bytes && !field.name.length && field.type==XR_XIR_UNIT);
    CHECK(xr_xir_compile_description_field(&c,&view,2,&field)==XR_XIR_OK && field.type==(XrXirType)257);
    view.field_count=4;
    CHECK(xr_xir_compile_description_field(&c,&view,3,&field)==XR_XIR_BAD_STRUCTURE);
    declaration.kind=XR_XIR_NOMINAL_CLASS;
    CHECK(xr_xir_compile_types_structure_verify(&c,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_description(&c,&types,(XrXirType)256,&view)==XR_XIR_OK && view.nominal_kind==XR_XIR_NOMINAL_CLASS);
    declaration.kind=XR_XIR_NOMINAL_STRUCT;
    budget_cases(&types);XrXirNominalTable *projected=NULL;
    CHECK(xr_xir_compile_nominal_project(&c,&table,&projected)==XR_XIR_OK);
    memset(names,0xcc,sizeof(names));memset(fields,0xcc,sizeof(fields));memset(&declaration,0xcc,sizeof(declaration));
    types.nominals=projected;
    CHECK(xr_xir_compile_types_structure_verify(&c,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_description(&c,&types,(XrXirType)256,&view)==XR_XIR_OK && !view.definition && view.identity);
    CHECK(xr_xir_compile_description_field(&c,&view,0,&field)==XR_XIR_OK && field.name.bytes[0]=='z');
    CHECK(xr_xir_compile_description_field(&c,&view,1,&field)==XR_XIR_OK && field.name.bytes[0]=='a' && field.type==XR_XIR_STRING);
    xr_xir_compile_nominal_free(projected);CHECK(stats(&c).live_bytes==baseline);xr_compile_resources_release(c.resources);
    puts("canonical type descriptions: all existing families, declaration order, tuple Unit, producer death, budget and failure preservation");
    return 0;
}
