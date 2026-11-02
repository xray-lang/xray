/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_hierarchy_contract.c - Ancestry validation across owned nominal forms
 *
 * KEY CONCEPT:
 *   A matching field shape cannot manufacture class identity or parent authority.
 */
#include "xir/xxir_nominal.h"
#include "xir/xxir_class_hierarchy_internal.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"hierarchy %d: %s\n",__LINE__,#c); exit(1); } } while (0)

typedef struct HierarchyFixture {
    XrXirNominalField fields[4][3];
    XrXirType field_types[4][3];
    XrXirNominalDeclaration declarations[4];
    XrXirTypeNode nodes[4];
    XrXirNominalTable table;
    XrXirTypes types;
} HierarchyFixture;

static void hierarchy_fixture(HierarchyFixture *fixture) {
    static const char *names[]={"Base","Derived","Leaf","Peer"};
    static const uint32_t lengths[]={4,7,4,4},counts[]={1,2,3,1},parents[]={0,1,2,0};
    const XrXirNominalField fields[]={{{"count",5},XR_XIR_I64,XR_XIR_FIELD_MUTABLE},
        {{"label",5},XR_XIR_STRING,0},{{"extra",5},XR_XIR_STRING,0}};
    memset(fixture,0,sizeof(*fixture));
    for(uint32_t d=0;d<4;++d) {
        for(uint32_t f=0;f<counts[d];++f) {
            fixture->fields[d][f]=fields[f];fixture->field_types[d][f]=fields[f].type;
        }
        fixture->declarations[d]=(XrXirNominalDeclaration){.module={"root",4},
            .name={names[d],lengths[d]},.exported=1,.fields=fixture->fields[d],
            .field_count=counts[d],.kind=XR_XIR_NOMINAL_CLASS,.parent=parents[d]};
        fixture->nodes[d]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
            .nominal={d,NULL,0,fixture->field_types[d],counts[d]}};
    }
    fixture->table=(XrXirNominalTable){fixture->declarations,4,NULL};
    fixture->types=(XrXirTypes){fixture->nodes,4,&fixture->table,NULL};
}

static XrXirStatus hierarchy_verify(const XrXirCompileContext *context,HierarchyFixture *fixture) {
    return xr_xir_compile_nominal_structure_verify(context,&fixture->table,&fixture->types);
}

static void hierarchy_parent_controls(const XrXirCompileContext *context) {
    HierarchyFixture fixture;hierarchy_fixture(&fixture);
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_OK);
    XrXirType base=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(xr_xir_class_is_subtype(&fixture.types,(XrXirType)(base+2),base));
    CHECK(xr_xir_class_is_subtype(&fixture.types,(XrXirType)(base+2),(XrXirType)(base+1)));
    CHECK(!xr_xir_class_is_subtype(&fixture.types,base,(XrXirType)(base+2)));
    CHECK(!xr_xir_class_is_subtype(&fixture.types,(XrXirType)(base+3),base));
    fixture.declarations[1].parent=5;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_STRUCTURE);
    fixture.declarations[1].parent=2;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_STRUCTURE);
    fixture.declarations[1].parent=1;fixture.declarations[0].parent=3;
    for(unsigned d=0;d<3;++d) {
        fixture.declarations[d].field_count=1;fixture.nodes[d].nominal.field_count=1;
    }
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_STRUCTURE);
    hierarchy_fixture(&fixture);fixture.declarations[0].flags=XR_XIR_NOMINAL_FINAL;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
    fixture.declarations[0].flags=0;fixture.declarations[1].kind=XR_XIR_NOMINAL_STRUCT;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
    fixture.declarations[1].kind=XR_XIR_NOMINAL_CLASS;fixture.declarations[0].kind=XR_XIR_NOMINAL_STRUCT;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
    fixture.declarations[0].kind=XR_XIR_NOMINAL_CLASS;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_OK);
}

static void hierarchy_prefix_controls(const XrXirCompileContext *context) {
    HierarchyFixture fixture;hierarchy_fixture(&fixture);
    fixture.fields[1][0].name=(XrXirLiteral){"other",5};
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_STRUCTURE);
    fixture.fields[1][0].name=(XrXirLiteral){"count",5};fixture.fields[1][0].flags=0;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_STRUCTURE);
    fixture.fields[1][0].flags=XR_XIR_FIELD_MUTABLE;
    fixture.fields[1][0].type=XR_XIR_STRING;fixture.field_types[1][0]=XR_XIR_STRING;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
    hierarchy_fixture(&fixture);
    fixture.declarations[1].fields=NULL;fixture.declarations[1].field_count=0;
    fixture.nodes[1].nominal.fields=NULL;fixture.nodes[1].nominal.field_count=0;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
    for(unsigned mode=0;mode<2;++mode) {
        hierarchy_fixture(&fixture);
        uint32_t access=mode?XR_XIR_FIELD_PROTECTED:XR_XIR_FIELD_PRIVATE;
        for(unsigned d=0;d<3;++d)fixture.fields[d][0].flags|=access;
        CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
        for(unsigned d=0;d<4;++d)fixture.declarations[d].parent=0;
        CHECK(hierarchy_verify(context,&fixture)==XR_XIR_OK);
    }
}

static void hierarchy_generic_and_sendable_controls(const XrXirCompileContext *context) {
    HierarchyFixture fixture;XrXirConstraint constraint={0};XrXirType argument=XR_XIR_I64;
    for(unsigned d=0;d<2;++d) {
        hierarchy_fixture(&fixture);
        fixture.declarations[d].parameter_count=1;fixture.declarations[d].constraints=&constraint;
        fixture.nodes[d].nominal.arguments=&argument;fixture.nodes[d].nominal.argument_count=1;
        CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
        for(unsigned n=0;n<4;++n)fixture.declarations[n].parent=0;
        CHECK(hierarchy_verify(context,&fixture)==XR_XIR_OK);
    }
    hierarchy_fixture(&fixture);
    for(unsigned d=0;d<3;++d)fixture.fields[d][0].flags=0;
    fixture.declarations[0].flags=XR_XIR_NOMINAL_SENDABLE;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_BAD_TYPE);
    for(unsigned d=0;d<4;++d)fixture.declarations[d].parent=0;
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_OK);
}

static void hierarchy_owned_forms(const XrXirCompileContext *context) {
    HierarchyFixture fixture;hierarchy_fixture(&fixture);
    CHECK(hierarchy_verify(context,&fixture)==XR_XIR_OK);
    XrXirNominalTable *clone=NULL,*projected=NULL;
    CHECK(xr_xir_compile_nominal_clone(context,&fixture.table,&fixture.types,&clone)==XR_XIR_OK);
    CHECK(xr_xir_compile_nominal_project(context,&fixture.table,NULL,&projected)==XR_XIR_OK);
    CHECK(clone && clone->declarations && projected && projected->identities);
    CHECK(clone->declarations[2].parent==2 && projected->identities[2].parent==2);
    fixture.declarations[0].name=(XrXirLiteral){"lost",4};
    fixture.declarations[1].parent=0;fixture.declarations[2].parent=0;
    fixture.fields[1][0].name=(XrXirLiteral){"lost!",5};
    CHECK(!memcmp(clone->declarations[0].name.bytes,"Base",4));
    CHECK(!memcmp(projected->identities[0].name.bytes,"Base",4));
    CHECK(!memcmp(clone->declarations[1].fields[0].name.bytes,"count",5));
    XrXirTypes owned=fixture.types;owned.nominals=clone;
    CHECK(xr_xir_compile_nominal_structure_verify(context,clone,&owned)==XR_XIR_OK);
    owned.nominals=projected;
    CHECK(xr_xir_compile_nominal_structure_verify(context,projected,&owned)==XR_XIR_OK);
    XrXirNominalIdentity *identities=(XrXirNominalIdentity *)projected->identities;
    identities[1].parent=5;
    CHECK(xr_xir_compile_nominal_structure_verify(context,projected,&owned)==XR_XIR_BAD_STRUCTURE);
    identities[1].parent=1;
    XrXirType original=fixture.field_types[1][0];fixture.field_types[1][0]=XR_XIR_STRING;
    CHECK(xr_xir_compile_nominal_structure_verify(context,projected,&owned)==XR_XIR_BAD_TYPE);
    fixture.field_types[1][0]=original;
    CHECK(xr_xir_compile_nominal_structure_verify(context,projected,&owned)==XR_XIR_OK);
    xr_xir_compile_nominal_free(projected);xr_xir_compile_nominal_free(clone);
}

int main(void) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    hierarchy_parent_controls(&context);hierarchy_prefix_controls(&context);
    hierarchy_generic_and_sendable_controls(&context);hierarchy_owned_forms(&context);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);
    puts("Class ancestry declaration/identity controls and owned projections PASS");return 0;
}
