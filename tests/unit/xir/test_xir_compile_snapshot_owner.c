/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_snapshot_owner.c - Snapshot copies retain the shared resource owner
 */
#include "xir_construction_fixture.h"
#include "base/xmalloc.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include <stdio.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, physical, peak, total;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer) ++i;
    CHECK(i < 4096);
    allocations[i] = (Allocation){memory, bytes};
    ++live; physical += bytes; total += bytes;
    if (physical > peak) peak = physical;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer != memory) ++i;
    CHECK(i < 4096 && live);
    size_t bytes = allocations[i].bytes;
    xr_free(memory);
    allocations[i] = (Allocation){0}; --live; physical -= bytes;
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"

static void reset_observer(void) {
    CHECK(!live && !physical);
    attempts = peak = total = 0; fail_at = SIZE_MAX;
}
static XrXirCompileContext context_new(uint64_t work) {
    XrXirCompileContext context = {0};
    const XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, work};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    return context;
}
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats value;
    CHECK(xr_compile_resources_stats(context->resources, &value) == XR_COMPILE_RESOURCE_OK);
    CHECK(value.live_bytes == physical && value.peak_bytes == peak && value.allocated_bytes == total);
    return value;
}

static XrXirStatus snapshot_fixture(const XrXirCompileContext *context, XrXirSourceSnapshot **out) {
    char name[]="compute", signature[]="compute(i64) -> string";
    char identity[]="root.identity", path[]="root.xr";
    XrXirSourceQueryModule module={identity,path,{0}};
    XrXirSourceType parameter={XR_XIR_I64,0,true};
    XrXirSourceDeclaration declaration={0};
    declaration.name=name; declaration.signature=signature;
    declaration.parameters=&parameter; declaration.parameter_count=1;
    declaration.type=(XrXirSourceType){XR_XIR_STRING,0,true};
    XrXirSourceReference reference={0}; reference.declaration=17;
    XrXirSourceExpression expression={0}; expression.node=31; expression.type=parameter;
    XrXirCallableParameter parameter_type={XR_XIR_I64,0};
    XrXirTypeNode node={0}; node.kind=XR_XIR_TYPE_CALLABLE; node.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    node.parameters=&parameter_type; node.parameter_count=1; node.result=XR_XIR_STRING;
    XrXirType argument=XR_XIR_I64;
    XrXirInterfaceApplication application={0,&argument,1};
    XrXirConstraint constraint={0,&application,1};
    uint32_t binder=XR_XIR_BINDER_TYPE;
    declaration.generic_constraints=&constraint; declaration.generic_parameter_count=1;
    declaration.type_parameter_kinds=&binder;
    XrXirNominalField field={{"value",5},XR_XIR_I64,0};
    XrXirNominalDeclaration nominal={0};
    nominal.module=(XrXirLiteral){identity,13}; nominal.name=(XrXirLiteral){name,7};
    nominal.constraints=&constraint; nominal.parameter_count=1;
    nominal.fields=&field; nominal.field_count=1;
    XrXirNominalVariant variant={{"Some",4},0,1};
    nominal.kind=XR_XIR_NOMINAL_ENUM; nominal.variants=&variant; nominal.variant_count=1;
    XrXirNominalTable nominals={&nominal,1,NULL};
    XrXirInterfaceMethod method={{name,7},(XrXirType)256,0,1,&constraint};
    XrXirInterfaceDeclaration interface={0};
    interface.module=(XrXirLiteral){identity,13};interface.name=(XrXirLiteral){"Show",4};
    interface.constraints=&constraint;interface.parameter_count=1;
    interface.methods=&method;interface.method_count=1;
    XrXirInterfaceTable interfaces={&interface,1};
    XrXirImplementationBinding binding={application,0,2};
    XrXirImplementation implementation={0,application,&binding,1};
    XrXirImplementationTable implementations={&implementation,1};
    XrXirTypes types={&node,1,&nominals,&interfaces};
    XrXirSourceView view={0}; view.complete=true;
    view.modules=&module; view.module_count=1;
    view.declarations=&declaration; view.declaration_count=1;
    view.references=&reference; view.reference_count=1;
    view.expressions=&expression; view.expression_count=1; view.types=&types;
    view.implementations=&implementations;
    return xir_fixture_snapshot_copy(context,&view,out);
}
static void snapshot_lifetime_and_failures(void) {
    size_t count=0; uint64_t needed=0;
    for (size_t failure=SIZE_MAX;;) {
        reset_observer();
        XrXirCompileContext context=context_new(UINT64_MAX);
        fail_at=failure;
        XrXirSourceSnapshot *snapshot=NULL;
        XrXirStatus status=snapshot_fixture(&context,&snapshot);
        if (failure==SIZE_MAX) {
            CHECK(status==XR_XIR_OK && snapshot); count=attempts; needed=stats(&context).work;
            xr_compile_resources_release(context.resources);
            const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
            CHECK(view && view->complete && view->module_count==1 && view->declaration_count==1);
            CHECK(!strcmp(view->modules[0].identity,"root.identity") && !strcmp(view->modules[0].path,"root.xr"));
            CHECK(!strcmp(view->declarations[0].name,"compute"));
            CHECK(!strcmp(view->declarations[0].signature,"compute(i64) -> string"));
            CHECK(view->declarations[0].parameters[0].type==XR_XIR_I64);
            CHECK(view->types->nodes[0].parameters[0].type==XR_XIR_I64 && view->types->nodes[0].result==XR_XIR_STRING);
            CHECK(view->types->nodes[0].flags==8u);
            CHECK(view->references[0].declaration==17 && view->expressions[0].node==31);
            CHECK(view->declarations[0].generic_constraints[0].interfaces[0].arguments[0]==XR_XIR_I64);
            CHECK(view->declarations[0].type_parameter_kinds[0]==XR_XIR_BINDER_TYPE);
            CHECK(!memcmp(view->types->nominals->declarations[0].fields[0].name.bytes,"value",5));
            CHECK(!memcmp(view->types->nominals->declarations[0].variants[0].name.bytes,"Some",4));
            CHECK(!memcmp(view->types->interfaces->declarations[0].methods[0].name.bytes,"compute",7));
            CHECK(view->types->interfaces->declarations[0].methods[0].constraints[0].interfaces[0].arguments[0]==XR_XIR_I64);
            CHECK(view->implementations->records[0].bindings[0].requirement.arguments[0]==XR_XIR_I64);
            xr_xir_compile_source_snapshot_free(snapshot);
        } else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !snapshot);
            CHECK(stats(&context).allocation_count>=1);
            xr_compile_resources_release(context.resources);
        }
        CHECK(!physical && !live);
        if (failure==SIZE_MAX) failure=1;
        else if (++failure==count) break;
    }
    for (uint64_t limit=1;limit<=needed;limit+=limit<needed && needed-limit>37 ? 37:1) {
        reset_observer(); XrXirCompileContext context=context_new(limit);
        XrXirSourceSnapshot *snapshot=NULL;
        XrXirStatus status=snapshot_fixture(&context,&snapshot);
        CHECK(status==(limit==needed?XR_XIR_OK:XR_XIR_BUDGET));
        xr_xir_compile_source_snapshot_free(snapshot); xr_compile_resources_release(context.resources);
        CHECK(!physical && !live);
    }
    printf("Source snapshot: %zu actual OOM points, work through %llu, physical zero\n",count-1,(unsigned long long)needed);
}
static void snapshot_output(void) {
    reset_observer(); XrXirCompileContext context=context_new(UINT64_MAX);
    XrXirSourceSnapshot *untouched=(XrXirSourceSnapshot *)(uintptr_t)1;
    CHECK(snapshot_fixture(&context,&untouched)==XR_XIR_BAD_STRUCTURE);
    CHECK(untouched==(XrXirSourceSnapshot *)(uintptr_t)1 && attempts==1);
    XrXirSourceView bad={0};bad.module_count=1;
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xir_fixture_snapshot_copy(&context,&bad,&snapshot)==XR_XIR_BAD_STRUCTURE && !snapshot);
    xr_compile_resources_release(context.resources);CHECK(!physical && !live);
}
static void snapshot_missing_nested_storage(void) {
    for (unsigned kind=0;kind<9;++kind) {
        reset_observer(); XrXirCompileContext context=context_new(UINT64_MAX);
        XrXirSourceView view={0}; XrXirSourceSnapshot *snapshot=NULL;
        XrXirSourceDeclaration declaration={0};
        XrXirTypeNode node={0};
        XrXirNominalDeclaration nominal={0};
        XrXirNominalTable nominals={&nominal,1,NULL};
        XrXirInterfaceDeclaration interface={0};
        XrXirInterfaceTable interfaces={&interface,1};
        XrXirTypes types={0}; view.types=&types;
        switch (kind) {
        case 0: declaration.parameter_count=1;view.declarations=&declaration;view.declaration_count=1;break;
        case 1: types.nodes=&node;types.count=1;node.parameter_count=1;break;
        case 2: types.nodes=&node;types.count=1;node.nominal.argument_count=1;break;
        case 3: types.nodes=&node;types.count=1;node.nominal.field_count=1;break;
        case 4: types.nominals=&nominals;nominals.declarations=NULL;break;
        case 5: types.nominals=&nominals;nominal.field_count=1;break;
        case 6: types.nominals=&nominals;nominal.variant_count=1;break;
        case 7: types.interfaces=&interfaces;interfaces.declarations=NULL;break;
        case 8: types.interfaces=&interfaces;interface.method_count=1;break;
        }
        CHECK(xir_fixture_snapshot_copy(&context,&view,&snapshot)==XR_XIR_BAD_STRUCTURE);
        CHECK(!snapshot && stats(&context).allocation_count>=1);
        xr_compile_resources_release(context.resources);CHECK(!physical && !live);
    }
}
static void snapshot_fixed_work(void) {
    _Static_assert(sizeof(XrXirSourceView)==296,"Independent source fact layout");
    /* x64: ledger 1 + input facts calloc 57 + view admission 1 + facts shape 1
     * + snapshot calloc 345 + public view copy 296 + clone shape 1
     * + receiving new shape 1 + receiving facts calloc 57 = 760. */
    _Static_assert(sizeof(XrXirConstruction)==56,"Independent private facts layout");
    for (uint64_t work=759;work<=760;++work) {
        reset_observer(); XrXirCompileContext context=context_new(work);
        XrXirSourceView view={0}; XrXirSourceSnapshot *snapshot=NULL;
        CHECK(xir_fixture_snapshot_copy(&context,&view,&snapshot)==
            (work==760?XR_XIR_OK:XR_XIR_BUDGET));
        if (snapshot) CHECK(stats(&context).work==760 && attempts==4);
        xr_xir_compile_source_snapshot_free(snapshot);xr_compile_resources_release(context.resources);
        CHECK(!physical && !live);
    }
}
/* Observation snapshots copy their own facts after the original producer has
 * died. No query result is fed into an executable admission as authority. */
static void snapshot_recipient_lifetime(void) {
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass) {
        reset_observer();
        XrXirCompileContext producer=context_new(UINT64_MAX);
        XrXirSourceSnapshot *source=NULL,*receiver=NULL;
        CHECK(snapshot_fixture(&producer,&source)==XR_XIR_OK && source);
        xr_compile_resources_release(producer.resources);producer.resources=NULL;
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source);
        const XrXirConstruction *facts=xr_xir_compile_source_snapshot_construction(source);
        CHECK(facts && xr_xir_compile_construction_count(facts)==1);
        const XrXirConstructionRow *row=xr_xir_compile_construction_row(facts,0);
        CHECK(row && row->field_count==1 && !row->default_initializer && !row->field_initializers[0]);
        XrXirCompileContext context=context_new(UINT64_MAX);
        XrXirSourceSnapshot *occupied=source;size_t start=attempts;
        CHECK(xr_xir_compile_source_snapshot_copy_v2(&context,view,facts,&occupied)==XR_XIR_BAD_STRUCTURE && occupied==source && attempts==start);
        size_t before_live=live,before_bytes=physical;
        fail_at=pass ? start+pass-1 : SIZE_MAX;
        XrXirStatus status=xr_xir_compile_source_snapshot_copy_v2(&context,view,facts,&receiver);
        if(!pass) {
            CHECK(status==XR_XIR_OK && receiver);sites=attempts-start;CHECK(sites);
            const XrXirConstruction *copied=xr_xir_compile_source_snapshot_construction(receiver);
            CHECK(copied && copied!=facts && xr_xir_compile_construction_count(copied)==1);
            CHECK(xr_xir_compile_construction_row(copied,0)!=row);
            CHECK(xr_xir_compile_construction_row(copied,0)->field_initializers!=row->field_initializers);
            xr_xir_compile_source_snapshot_free(source);source=NULL;
            xr_compile_resources_release(context.resources);context.resources=NULL;
            const XrXirSourceView *retained=xr_xir_compile_source_snapshot_view(receiver);
            CHECK(retained && !strcmp(retained->modules[0].identity,"root.identity") && !strcmp(retained->declarations[0].name,"compute"));
            row=xr_xir_compile_construction_row(copied,0);
            CHECK(row && row->field_count==1 && !row->field_initializers[0]);
        } else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !receiver && attempts==start+pass);
            CHECK(live==before_live && physical==before_bytes);
            CHECK(xr_xir_compile_source_snapshot_construction(source)==facts && !row->field_initializers[0]);
            xr_compile_resources_release(context.resources);context.resources=NULL;
        }
        fail_at=SIZE_MAX;xr_xir_compile_source_snapshot_free(source);xr_xir_compile_source_snapshot_free(receiver);
        CHECK(!live && !physical);
    }
    printf("Snapshot recipient after producer death: actual copy OOM sites=%zu physical zero\n",sites);
}
int main(void) { snapshot_lifetime_and_failures(); snapshot_output(); snapshot_missing_nested_storage(); snapshot_fixed_work(); snapshot_recipient_lifetime(); return 0; }
