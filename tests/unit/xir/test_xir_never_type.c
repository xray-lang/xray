/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_never_type.c - Empty error expressions across compiler ownership
 *
 * KEY CONCEPT:
 *   Error constraints admit Never without inventing a value or storage layout.
 */
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include "base/xcompile_resources.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr,"FAIL %d: %s\n",__LINE__,#expression); exit(1); \
} } while (0)

typedef struct NeverFixture {
    XrXirConstraint constraint;
    XrXirNominalVariant variant;
    XrXirNominalDeclaration declaration;
    XrXirNominalTable table;
    XrXirType argument;
    XrXirTypeNode node;
    XrXirTypes types;
} NeverFixture;

static void fixture_init(NeverFixture *f) {
    *f=(NeverFixture){0};
    f->constraint.markers=XR_XIR_CONSTRAINT_ERROR;
    f->variant=(XrXirNominalVariant){{"Empty",5},0,0};
    f->declaration=(XrXirNominalDeclaration){.module={"test",4},.name={"ErrorTag",8},
        .constraints=&f->constraint,.parameter_count=1,.kind=XR_XIR_NOMINAL_ENUM,
        .variants=&f->variant,.variant_count=1};
    f->table=(XrXirNominalTable){&f->declaration,1,NULL};
    f->argument=XR_XIR_NEVER;
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.arguments=&f->argument,.argument_count=1}};
    f->types=(XrXirTypes){&f->node,1,&f->table,NULL};
}

static void check_error_expression(const XrXirCompileContext *work,const XrXirTypes *types) {
    XrXirModule module={.types=types};
    XrXirProofContext closed={&module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xr_xir_compile_type_expression_shape(work,types,XR_XIR_NEVER,0)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(work,&closed,XR_XIR_NEVER,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_markers_prove(work,&closed,XR_XIR_I64,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_markers_prove(work,&closed,XR_XIR_UNIT,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
    XrXirTypeDescription description={0};
    CHECK(xr_xir_compile_type_description(work,types,XR_XIR_NEVER,&description)==XR_XIR_OK);
    CHECK(description.type==XR_XIR_NEVER && !description.kind && !description.node);

    XrXirType argument=XR_XIR_NEVER;
    XrXirConstraintUse use={&module,{XR_XIR_CONTEXT_NOMINAL,0,0},0,&argument,1};
    CHECK(xr_xir_compile_constraints_prove(work,&closed,&use)==XR_XIR_OK);
    XrXirProofContext formal={&module,{XR_XIR_CONTEXT_NOMINAL,0,0}};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    CHECK(xr_xir_compile_type_markers_prove(work,&formal,parameter,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches(work,types,&argument,1,parameter,XR_XIR_NEVER)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_substitution_matches(work,types,&argument,1,parameter,XR_XIR_UNIT)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_substitution_matches(work,types,&argument,1,XR_XIR_NEVER,XR_XIR_ERROR)==XR_XIR_BAD_TYPE);
    argument=XR_XIR_I64;
    CHECK(xr_xir_compile_constraints_prove(work,&closed,&use)==XR_XIR_BAD_TYPE);
}

static void check_no_value(const XrXirCompileContext *work,const XrXirTypes *types) {
    XrXirModule module={.types=types};
    XrXirProofContext closed={&module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    CHECK(xr_xir_compile_type_storage_prove(work,&closed,XR_XIR_NEVER)==XR_XIR_BAD_TYPE);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    for (uint32_t kind=XR_XIR_LAYOUT_STORAGE;kind<=XR_XIR_LAYOUT_FRAME;++kind) {
        XrXirLayout layout={123,8};
        CHECK(xr_xir_compile_layout(work,types,XR_XIR_NEVER,&target,(XrXirLayoutContext)kind,&layout)==XR_XIR_BAD_LAYOUT);
        CHECK(layout.size==123 && layout.alignment==8);
    }
    XrXirValue value={XR_XIR_NEVER,0,0};
    CHECK(!xr_xir_value_valid(&value));
    XrXirTypeNode array={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_NEVER};
    XrXirTypes invalid={&array,1,NULL,NULL};
    CHECK(xr_xir_compile_type_descriptors_verify(work,&invalid)==XR_XIR_BAD_TYPE);
}

int main(void) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext work={0};
    CHECK(xr_compile_resources_new(&limits,&work.resources)==XR_COMPILE_RESOURCE_OK);
    work.limits=xr_xir_compile_default_limits();
    NeverFixture *producer=xr_malloc(sizeof(*producer));
    CHECK(producer);
    fixture_init(producer);
    CHECK(xr_xir_compile_type_descriptors_verify(&work,&producer->types)==XR_XIR_OK);
    check_error_expression(&work,&producer->types);
    check_no_value(&work,&producer->types);
    XrXirTypes *owned=NULL;
    CHECK(xr_xir_compile_types_clone(&work,&producer->types,&owned)==XR_XIR_OK);
    xr_free(producer);
    CHECK(owned && owned->nodes[0].nominal.arguments[0]==XR_XIR_NEVER);
    CHECK(xr_xir_compile_type_descriptors_verify(&work,owned)==XR_XIR_OK);
    check_error_expression(&work,owned);
    check_no_value(&work,owned);
    xr_xir_compile_types_free(owned);
    xr_compile_resources_release(work.resources);
    puts("Never error constraints, substitution, owned clone and value rejection passed");
    return 0;
}
